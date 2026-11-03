#include "GameBridge.h"

#include "AnimEvents.h"
#include "AnimProbe.h"
#include "MoveControls.h"
#include "NpcSync.h"
#include "Overlay.h"
#include "SessionControl.h"
#include "SteamInvite.h"
#include "Avatars.h"
#include "GameState.h"

#include "cosync/net/Util.h"
#include "cosync/shared/Log.h"

#include <RE/Starfield.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace cosync::plugin
{
    namespace
    {
        constexpr std::size_t kMaxEventsPerFrame = 256;  // bounds per-frame work
        constexpr auto kSampleInterval = std::chrono::milliseconds(33);  // ~30 Hz
        constexpr auto kLocalLogInterval = std::chrono::seconds(5);
        constexpr auto kSummaryInterval = std::chrono::seconds(5);
        constexpr auto kAvatarMoveInterval = std::chrono::milliseconds(100);  // Papyrus calls; 10 Hz
        constexpr auto kAvatarRetryInterval = std::chrono::seconds(5);
        constexpr auto kAvatarSpawnTimeout = std::chrono::seconds(10);  // e.g. VM frozen during a load
        constexpr std::uint32_t kAvatarLoggedAttempts = 6;              // keep retrying, but quietly
        constexpr auto kVmSettleTime = std::chrono::seconds(2);           // VM must be running this long first
        constexpr std::uint32_t kTemplateMaxTries = 16;  // FindRandomActor may return the player or leveled NPCs

        // Graph input events replayed on proxies (names observed on the player's graph in game,
        // 2026-11-03, docs/ANIMATION_REVERSE_ENGINEERING.md A.11). Equip/Unequip are left out until
        // equipment is synchronised: the proxy holds different gear.
        constexpr std::string_view kReplayedEvents[] = {
            "JumpStart",    "JumpLand",          "SneakStart",      "SneakStop",  "MeleeAttackStart",
            "FireSingleStart", "FireRelease",    "SightedStart",    "SightedEnd", "SightedStopRequest",
            "blockStart",   "blockStop",         "RelaxedStart",    "MeleeAttackFinisherStart",
        };
        bool Replayed(std::string_view name)
        {
            return std::ranges::find(kReplayedEvents, name) != std::end(kReplayedEvents);
        }
        // Wrapping u16 sequence comparison.
        bool Newer16(std::uint16_t candidate, std::uint16_t last)
        {
            return candidate != last && static_cast<std::int16_t>(candidate - last) > 0;
        }
    }

    GameBridge& GameBridge::Get()
    {
        static GameBridge instance;
        return instance;
    }

    void GameBridge::UpdateUi()
    {
        constexpr std::chrono::steady_clock::time_point kNever{};
        const auto now = std::chrono::steady_clock::now();
        auto* worker = _worker.load();
        if (now >= _nextUiUpdate) {
            _nextUiUpdate = now + std::chrono::milliseconds(200);
            if (now >= _nextSteamCheck) {
                _nextSteamCheck = now + std::chrono::seconds(10);
                _steamAvailable = steam::Available();
            }
            std::string code;
            if (worker) {
                const auto st = worker->Status();
                std::string lan;
                if (worker->IsHost()) {
                    if (_localIp.empty() && now >= _nextIpTry) {
                        _nextIpTry = now + std::chrono::seconds(30);
                        _localIp = steam::LocalIPv4();
                    }
                    if (!_localIp.empty()) lan = std::format("{}:{}", _localIp, worker->ListenPort());
                    // Primary code: Steam (relay, works across the internet) when available, else LAN.
                    code = _steamAvailable ? steam::SteamJoinCode() : std::string();
                    if (code.empty()) code = lan;
                }
                _ui.SetLanCode(lan);
                _ui.SetConnection(st.state, "", worker->IsHost(), "", code, st.localPlayer, st.rttMs, st.droppedEvents);
                std::vector<ui::PlayerRow> rows;
                auto roster = worker->Roster();
                if (st.localPlayer != kInvalidPlayerId && !roster.contains(st.localPlayer)) roster[st.localPlayer] = "You";
                for (const auto& [pid, name] : roster) {
                    ui::PlayerRow r;
                    r.id = pid;
                    r.name = name;
                    r.isLocal = pid == st.localPlayer;
                    r.isHost = r.isLocal && worker->IsHost();
                    r.proxy = "none";
                    if (auto it = _remotes.find(pid); it != _remotes.end() && it->second.avatar.refId != 0) {
                        r.sameLocation = true;
                        r.proxy = it->second.avatar.loaded3DAt != kNever ? "valid" : "spawning";
                    }
                    rows.push_back(std::move(r));
                }
                _ui.SetPlayers(std::move(rows));
            } else {
                _ui.SetConnection(net::ClientState::Idle, "", false, "", "", kInvalidPlayerId, 0, 0);
                _ui.SetPlayers({});
                _ui.SetLanCode("");
            }
            // LAN browse results.
            if (_discovery.valid() && _discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                std::vector<ui::ServerRow> rows;
                for (const auto& d : _discovery.get()) {
                    ui::ServerRow r;
                    r.name = d.info.serverName;
                    r.address = d.endpoint.ToString();
                    r.players = d.info.players;
                    r.maxPlayers = d.info.maxPlayers;
                    r.pingMs = d.latencyMs;
                    r.passworded = d.info.passwordRequired;
                    rows.push_back(std::move(r));
                }
                _ui.SetServers(std::move(rows));
            }
            if (code != _joinCode) {
                _joinCode = code;
                if (_steamAvailable) steam::SetJoinPresence(code.empty() ? std::string() : "+cosync_join " + code);
            }
            _ui.SetJoinCode(_joinCode, _steamAvailable);
            _ui.SetVersions(std::format("{}.{}", proto::kProtocolMajor, proto::kProtocolMinor), "1.16.244");
        }
        if (_ui.Snapshot().version != _publishedUiVersion) {
            _publishedUiVersion = _ui.Snapshot().version;
            overlay::Publish(_ui.Snapshot());
        }

        // An accepted Steam invite (or a launch from one) overrides any join code: leave and join.
        if (const auto invite = steam::TakePendingJoin(); !invite.empty()) {
            std::string error;
            log::Info("[ui] accepted Steam invite: joining {}", invite);
            if (!session::Join(invite, "", error)) log::Warn("[ui] invite join failed: {}", error);
            _ui.SetError(error);
        }

        std::vector<ui::UiCommand> commands;
        overlay::DrainCommands(commands);
        for (const auto& c : commands) {
            if (_ui.ApplyLocal(c)) continue;
            std::string error;
            if (const auto* h = std::get_if<ui::cmd::HostGame>(&c)) {
                log::Info("[ui] host '{}' on port {}", h->serverName, h->port);
                session::Host(h->serverName, h->port, h->maxPlayers, h->password, error);
            } else if (const auto* j = std::get_if<ui::cmd::JoinGame>(&c)) {
                log::Info("[ui] join {}", j->address);
                session::Join(j->address, j->password, error);
            } else if (std::holds_alternative<ui::cmd::LeaveGame>(c)) {
                log::Info("[ui] leave");
                session::Leave();
            } else if (const auto* k = std::get_if<ui::cmd::KickPlayer>(&c)) {
                auto* w = _worker.load();
                if (w && w->IsHost()) {
                    log::Info("[ui] kick player {}", k->player);
                    w->SubmitKick(k->player);
                } else {
                    error = "Only the host can kick players.";
                }
            } else if (std::holds_alternative<ui::cmd::InviteFriends>(c)) {
                if (_joinCode.empty()) error = "Host a game first.";
                else if (!steam::OpenInviteDialog("+cosync_join " + _joinCode)) error = "Steam overlay not available.";
            } else if (std::holds_alternative<ui::cmd::RefreshServers>(c)) {
                if (!_discovery.valid()) {
                    // Blocking UDP query on a worker thread (no engine or session state touched).
                    _discovery = std::async(std::launch::async, [] {
                        std::string err;
                        const std::vector<std::string> local{ "127.0.0.1" };  // same-PC hosts
                        return net::discovery::Discover(net::discovery::kDefaultPort, 700, true, local, err);
                    });
                }
            } else if (std::holds_alternative<ui::cmd::CopyJoinCode>(c)) {
                if (_joinCode.empty() || !steam::CopyToClipboard(_joinCode)) error = "No join code to copy.";
            }
            _ui.SetError(error);
            if (!error.empty()) log::Warn("[ui] {}", error);
        }
    }

    void GameBridge::Detach()
    {
        RemoveAllRemotes();
        _worker.store(nullptr);
    }

    void GameBridge::Tick()
    {
        UpdateUi();  // first: a UI host/join/leave replaces the worker
        auto* worker = _worker.load();
        if (!worker) {
            return;
        }

        if (worker->ConsumeOverflow()) {
            log::Warn("game thread fell behind network events; rebuilding remote player list");
            RebuildFromRoster(*worker);
        }

        _events.clear();
        worker->DrainEvents(_events, kMaxEventsPerFrame);
        for (const auto& ev : _events) {
            std::visit(
                [this](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    namespace ce = net::client_event;
                    if constexpr (std::is_same_v<T, ce::StateChanged>) {
                        log::Info("[game] session state: {} ({})", net::ToString(e.state), e.detail);
                        if (e.state == net::ClientState::Disconnected) {
                            RemoveAllRemotes();
                        }
                    } else if constexpr (std::is_same_v<T, ce::Welcomed>) {
                        log::Info("[game] joined '{}' as player {}", e.welcome.serverName, e.welcome.playerId);
                    } else if constexpr (std::is_same_v<T, ce::Rejected>) {
                        log::Warn("[game] session refused: {} ({})", proto::ToString(e.reject.reason), e.reject.detail);
                    } else if constexpr (std::is_same_v<T, ce::PeerJoined>) {
                        _remotes[e.player.playerId].name = e.player.name;
                        log::Info("[game] player {} '{}' joined", e.player.playerId, e.player.name);
                    } else if constexpr (std::is_same_v<T, ce::NpcCombat>) {
                        ApplyNpcCombat(e.combat, *_worker.load());
                    } else if constexpr (std::is_same_v<T, ce::PeerLeft>) {
                        RemoveRemote(e.playerId);
                        log::Info("[game] player {} left ({})", e.playerId, proto::ToString(e.reason));
                    }
                },
                ev);
        }

        if (_readLocalPlayer) {
            SampleLocalPlayer(*worker);
        }
        UpdateRemotePlayers(*worker);
        if (_avatarsEnabled) UpdateNpcCombat(*worker);
        LogSummary(*worker);
    }

    void GameBridge::SampleLocalPlayer(net::NetworkWorker& worker)
    {
        const auto now = std::chrono::steady_clock::now();
        const bool newEvent = !_outEvents.empty() && _outEvents.back().sendsLeft == 3;
        if (now < _nextSample && !newEvent) {
            return;
        }
        _nextSample = now + kSampleInterval;

        const auto sample = ReadLocalPlayer();
        if (sample.has_value() != _inWorld) {
            _inWorld = sample.has_value();
            log::Info("[game] local player {}", _inWorld ? "in world; sending position" : "not in world; position paused");
        }
        if (!sample) {
            return;
        }
        if (sample->locationId != _localLocationId) {
            _localLocationId = sample->locationId;
            _localLocationName = sample->locationName;
            log::Info("[game] local location: {} (id {:08X}, cell FormID {:08X})", sample->locationName,
                      sample->locationId, sample->cellFormId);
        }
        // Engine movement input of the local player (captured by MoveControls), only while it keeps
        // updating: a stale capture (menus, loading) must not keep a remote proxy walking.
        std::optional<proto::PlayerControls> controls;
        if (_avatarsEnabled && (_opt.controls || _opt.mirror)) {
            const auto c = movecontrols::LatestPlayerControls();
            if (c.seq != _sentControlsSeq) {
                _sentControlsSeq = c.seq;
                _sentControlsAt = now;
            }
            if (c.seq != 0 && now - _sentControlsAt < std::chrono::milliseconds(250)) {
                proto::PlayerControls pc;
                std::memcpy(pc.values.data(), c.data, sizeof(float) * proto::kControlFloats);
                // Not written by the engine's producers (RE A.10): +0x0C, +0x20..+0x2F, +0x38, +0x3C,
                // +0x44, +0x4C, +0x50 carry stale stack bytes (2026-11-03: 1.4e31 at +0x38).
                for (int i : { 3, 8, 9, 10, 11, 14, 15, 17, 19, 20 }) pc.values[i] = 0.0f;
                pc.flag = static_cast<std::uint8_t>(c.data[proto::kControlFloats * sizeof(float)]) ? 1 : 0;
                if (proto::IsValidControls(pc)) controls = pc;
            }
        }
        if (_avatarsEnabled && _opt.controls && _vmSettled) {
            if (_weaponQuery && _weaponQuery->pending.load() <= 0) {
                const int drawn = _weaponQuery->drawn.load();
                if (drawn >= 0) {
                    const proto::WeaponState ws{ _weaponQuery->formId.load(), drawn == 1 };
                    if (!_localWeapon || _localWeapon->formId != ws.formId || _localWeapon->drawn != ws.drawn) {
                        log::Info("[weapon] local: {:08X} {}", ws.formId, ws.drawn ? "drawn" : "holstered");
                    }
                    _localWeapon = ws;
                }
                _weaponQuery.reset();
            }
            if (!_weaponQuery && now >= _nextWeaponQuery) {
                _nextWeaponQuery = now + std::chrono::milliseconds(500);
                auto q = std::make_shared<avatars::WeaponQuery>();
                if (avatars::RequestWeaponState(avatars::PlayerRefId(), q)) _weaponQuery = std::move(q);
            }
        }
        std::vector<proto::AnimEvent> events;
        for (auto& e : _outEvents) {
            if (events.size() >= proto::kMaxAnimEventsPerTransform) break;
            events.push_back({ e.sequence, e.name });
            --e.sendsLeft;
        }
        while (!_outEvents.empty() && _outEvents.front().sendsLeft <= 0) _outEvents.pop_front();
        Vec3 rotation = sample->rotation;
        if (_opt.controls) {
            float pitch = 0.0f;  // fCameraPitch: the player's look pitch (radians, -0.76..1.18 observed)
            if (animprobe::ReadFloat(avatars::PlayerRefId(), "fCameraPitch", pitch) && std::isfinite(pitch)) {
                rotation.x = std::clamp(pitch, -1.6f, 1.6f);
            }
        }
        worker.SubmitLocalTransform(sample->locationId, sample->position, rotation, controls, events,
                                    _opt.controls ? _localWeapon : std::nullopt);

        if (now >= _nextLocalLog) {
            _nextLocalLog = now + kLocalLogInterval;
            log::Info("[game] local: {} pos ({:.1f}, {:.1f}, {:.1f}) yaw {:.2f}", _localLocationName,
                      sample->position.x, sample->position.y, sample->position.z, sample->rotation.z);
        }
    }

    void GameBridge::UpdateRemotePlayers(net::NetworkWorker& worker)
    {
        _samples.clear();
        worker.TakeRemoteTransforms(_samples);
        for (const auto& s : _samples) {
            if (auto it = _remotes.find(s.player); it != _remotes.end()) {
                if (it->second.interpolator.Push(s.transform)) {
                    ++it->second.samplesReceived;
                }
                auto& r = it->second;
                if (!r.haveLatest || proto::SequenceNewer(s.transform.sequence, r.latestSeq)) {
                    r.latestYaw = s.transform.rotation.z;
                    r.latestPitch = s.transform.rotation.x;
                    if (r.haveLatest && s.transform.receivedMs > r.latestReceivedMs + 10) {
                        const float dt = static_cast<float>(s.transform.receivedMs - r.latestReceivedMs) / 1000.0f;
                        const float k = 0.5f;  // light smoothing
                        r.velX += ((s.transform.position.x - r.latestPos.x) / dt - r.velX) * k;
                        r.velY += ((s.transform.position.y - r.latestPos.y) / dt - r.velY) * k;
                        r.velZ += ((s.transform.position.z - r.latestPos.z) / dt - r.velZ) * k;
                    }
                    r.latestPos = s.transform.position;
                    r.latestReceivedMs = s.transform.receivedMs;
                    if (s.transform.weapon) r.weapon = s.transform.weapon;
                    r.latestSeq = s.transform.sequence;
                    r.haveLatest = true;
                }
                if (s.transform.controls &&
                    (r.controlsReceived == 0 || proto::SequenceNewer(s.transform.sequence, r.controlsSeq))) {
                    r.controls = s.transform.controls;
                    r.controlsSeq = s.transform.sequence;
                    r.controlsAt = std::chrono::steady_clock::now();
                    ++r.controlsReceived;
                }
                for (const auto& e : s.transform.events) {
                    if (r.anyEventSeen && !Newer16(e.sequence, r.lastEventSeq)) continue;
                    r.anyEventSeen = true;
                    r.lastEventSeq = e.sequence;
                    if (r.pendingReplays.size() < 16) r.pendingReplays.push_back(e.name);
                }
            }
        }

        // Same clock domain as RemoteSample::receivedMs (stamped on the network thread).
        const auto nowMs = net::SteadyNowMs();
        if (_avatarsEnabled) {
            UpdateVmState();
            UpdateTemplate();
            DrainAnimEvents();
            LogNpcEnumeration();
            if (_opt.animDiag) UpdateAnimDiag();
        }
        for (auto& [id, remote] : _remotes) {
            remote.pose = remote.interpolator.Sample(nowMs);
            if (_avatarsEnabled) {
                UpdateAvatar(id, remote);
            }
        }
        ReapOrphans();
        CheckDespawns();
    }

    void GameBridge::UpdateVmState()
    {
        const bool ready = avatars::VmReady();
        const auto now = std::chrono::steady_clock::now();
        if (ready && !_vmReady) {
            _vmReadySince = now;
            log::Info("[avatar] Papyrus VM running");
        } else if (!ready && _vmReady) {
            log::Info("[avatar] Papyrus VM frozen (loading); spawns paused");
            // Calls sent before the freeze are dropped by it: stop waiting for them.
            for (auto& [id, remote] : _remotes) {
                if (remote.avatar.pending) {
                    _orphans.push_back(std::move(remote.avatar.pending));
                    remote.avatar.nextAttempt = {};
                }
            }
        }
        _vmReady = ready;
        _vmSettled = ready && now - _vmReadySince >= kVmSettleTime;
    }

    bool GameBridge::TemplateDecided() const noexcept
    {
        return !_opt.nearbyBase || _templateBase != 0 || _templateTries >= kTemplateMaxTries;
    }

    void GameBridge::UpdateTemplate()
    {
        // Find one nearby NPC (not the player) and its data-file base, which all
        // avatars then copy. Two VM queries: FindRandomActor, then GetBaseObject.
        if (TemplateDecided() || !_vmSettled || !_inWorld) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (_templateQuery) {
            const auto result = _templateQuery->load();
            if (result == 0) {
                if (now - _templateQueriedAt > kAvatarSpawnTimeout) _templateQuery.reset();  // dropped; retry
                return;
            }
            _templateQuery.reset();
            if (_templateActor == 0) {
                // Phase 1 answered: a random nearby actor.
                if (!avatars::IsSpawnError(result) && result != avatars::PlayerRefId()) {
                    _templateActor = result;
                    _nextTemplateTry = {};  // ask for its base right away
                }
            } else {
                // Phase 2 answered: that actor's base as scripts see it.
                if (avatars::IsPlaceableBase(result)) {
                    _templateBase = result;
                    log::Info("[avatar] template: actor {:08X}, base NPC {:08X} (try {})", _templateActor, result,
                              _templateTries);
                } else {
                    log::Info("[avatar] actor {:08X} skipped: base {:08X} not placeable", _templateActor, result);
                    _templateActor = 0;
                }
            }
            if (_templateBase == 0 && _templateTries >= kTemplateMaxTries) {
                log::Warn("[avatar] no placeable NPC base found after {} tries; avatars will use the player base",
                          _templateTries);
            }
            return;
        }
        if (now < _nextTemplateTry) {
            return;
        }
        auto slot = std::make_shared<std::atomic<std::uint32_t>>(0);
        std::string error;
        bool dispatched = false;
        if (_templateActor == 0) {
            ++_templateTries;
            _nextTemplateTry = now + std::chrono::seconds(2);
            dispatched = avatars::RequestTemplateActor(slot, error);
        } else {
            dispatched = avatars::RequestBaseObject(_templateActor, slot, error);
            if (!dispatched) _templateActor = 0;
        }
        if (dispatched) {
            _templateQuery = std::move(slot);
            _templateQueriedAt = now;
        } else {
            log::Warn("[avatar] template query not dispatched: {}", error);
        }
    }

    void GameBridge::UpdateAvatar(PlayerId id, RemotePlayer& remote)
    {
        auto& avatar = remote.avatar;
        const auto now = std::chrono::steady_clock::now();
        // Graph events that arrive while no prepared proxy exists are dropped, never replayed late.
        if (avatar.refId == 0 || !avatar.motionApplied) remote.pendingReplays.clear();

        // Spawn result published by the VM thread?
        if (avatar.pending) {
            const auto result = avatar.pending->load();
            if (result == 0 && now - avatar.requestedAt > kAvatarSpawnTimeout) {
                if (avatar.attempts <= kAvatarLoggedAttempts) {
                    log::Warn("[avatar] player {} '{}': no reply from VM within 10 s; retrying", id, remote.name);
                }
                _orphans.push_back(std::move(avatar.pending));  // despawn it if it ever arrives
            } else if (result != 0) {
                avatar.pending.reset();
                if (avatars::IsSpawnError(result)) {
                    if (avatar.attempts <= kAvatarLoggedAttempts) {
                        log::Warn("[avatar] player {} '{}': spawn failed ({}): {}", id, remote.name, avatar.baseDetail,
                                  avatars::DescribeSpawnError(result));
                    }
                } else {
                    avatar.refId = result;
                    avatar.frozen = _opt.freezeMode == 1 && avatars::Freeze(avatar.refId);
                    avatar.loaded3DAt = {};
                    avatar.lastPosAt = {};
                    avatar.speed = 0.0f;
                    avatar.sentSpeed = -1.0f;
                    avatar.sentDirection = -1.0f;
                    avatar.nextAnimWrite = {};
                    avatar.probed = false;
                    avatar.motionApplied = false;
                    avatar.stateFixes = 0;
                    avatar.snaps = 0;
                    avatar.haveBodyYaw = false;
                    avatar.appliedWeapon = 0;
                    avatar.appliedDrawn = -1;
                    avatar.lastPathResult = 0;
                    if (_avatarScale != 1.0f) avatars::SetScale(avatar.refId, _avatarScale);
                    log::Info("[avatar] player {} '{}': spawned ref {:08X} (AI {}); right after spawn: {}", id,
                              remote.name, avatar.refId,
                              avatar.frozen          ? "disabled at spawn"
                              : _opt.freezeMode == 2 ? "on until 3D has loaded"
                                                     : "left on",
                              avatars::Describe(avatars::Probe(avatar.refId)));
                    avatar.probes = 0;
                    avatar.nextProbe = now + std::chrono::milliseconds(250);
                }
            }
        }

        // Same cell, or both outdoors (bit 31 of the location key) and close: exterior cell borders
        // must not despawn the proxy.
        bool visibleHere = false;
        if (_inWorld && remote.pose && !remote.pose->stale && _localLocationId != 0) {
            visibleHere = remote.pose->locationId == _localLocationId;
            if (!visibleHere && (remote.pose->locationId & _localLocationId & 0x80000000u) != 0) {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    const auto p = player->GetPosition();
                    const float dx = remote.pose->position.x - p.x, dy = remote.pose->position.y - p.y;
                    visibleHere = dx * dx + dy * dy < 1000.0f * 1000.0f;
                }
            }
        }
        if (!visibleHere) {
            ReleaseAvatar(id, avatar, "not in your cell");
            return;
        }

        if (avatar.refId == 0 && !avatar.pending && _vmSettled && TemplateDecided() && now >= avatar.nextAttempt) {
            ++avatar.attempts;
            // Back off: 5 s, 10 s, ... capped at 30 s.
            avatar.nextAttempt = now + std::min(kAvatarRetryInterval * avatar.attempts, std::chrono::seconds(30));
            // A nearby NPC's base renders fully; RequestSpawn falls back to the player base.
            auto slot = std::make_shared<std::atomic<std::uint32_t>>(0);
            std::string error;
            std::string detail;
            if (avatars::RequestSpawn(_templateBase, slot, error, detail)) {
                avatar.pending = std::move(slot);
                avatar.requestedAt = now;
                avatar.baseDetail = detail;
                if (avatar.attempts <= kAvatarLoggedAttempts) {
                    log::Info("[avatar] player {} '{}': spawn requested, attempt {} ({})", id, remote.name,
                              avatar.attempts, detail);
                }
            } else if (avatar.attempts <= kAvatarLoggedAttempts) {
                log::Warn("[avatar] player {} '{}': spawn not dispatched: {}", id, remote.name, error);
            }
        }

        // Validation: probe every 250 ms until 3D is loaded (measures the 3D load time), then
        // log at roughly 1, 2, 4 and 8 s after that and every 15 s afterwards.
        constexpr std::chrono::steady_clock::time_point kNever{};
        if (avatar.refId != 0 && now >= avatar.nextProbe) {
            const auto state = avatars::Probe(avatar.refId);
            if (state.has3D && avatar.loaded3DAt == kNever) {
                avatar.loaded3DAt = now;
                log::Info("[avatar] player {} '{}': ref {:08X} 3D loaded {} ms after the spawn request", id,
                          remote.name, avatar.refId,
                          std::chrono::duration_cast<std::chrono::milliseconds>(now - avatar.requestedAt).count());
            }
            if (avatar.loaded3DAt == kNever) {
                avatar.nextProbe = now + std::chrono::milliseconds(250);
            } else {
                ++avatar.probes;
                const auto delay =
                    avatar.probes < 4 ? std::chrono::seconds(1LL << avatar.probes) : std::chrono::seconds(15);
                avatar.nextProbe = now + delay;
                const bool ok =
                    state.isActor && !state.disabled && !state.deleted && state.has3D && state.sameCellAsPlayer;
                log::Info("[avatar] player {} '{}': ref {:08X} {} [{}]", id, remote.name, avatar.refId,
                          ok ? "VALID" : "INCOMPLETE", avatars::Describe(state));
            }
        }

        // Freeze mode 2: disable AI only after the 3D has been loaded for freezeDelay seconds,
        // so the engine's own post-load work (suspected fade-in) can finish first.
        if (_opt.freezeMode == 2 && avatar.refId != 0 && !avatar.frozen && avatar.loaded3DAt != kNever &&
            now - avatar.loaded3DAt >= std::chrono::duration<float>(_opt.freezeDelay)) {
            avatar.frozen = avatars::Freeze(avatar.refId);
            log::Info("[avatar] player {} '{}': ref {:08X} AI disabled {:.1f} s after 3D load", id, remote.name,
                      avatar.refId, _opt.freezeDelay);
        }

        // Experiment (AvatarMoveMode=motion): once the 3D has loaded, re-initialise the proxy's
        // movement controller through the engine's own path with motionDriven=true, the mode the
        // engine forces on the player (RE A.5). Applied once; the probe logs any later reversion.
        if (_opt.motion && avatar.refId != 0 && !avatar.motionApplied && avatar.loaded3DAt != kNever &&
            now - avatar.loaded3DAt >= std::chrono::seconds(1)) {
            avatar.motionApplied = true;
            std::string detail;
            const bool ok = animprobe::ReinitMovementController(avatar.refId, true, detail);
            log::Info("[avatar] player {} '{}': motion-driven controller {}: {}", id, remote.name,
                      ok ? "requested" : "NOT applied", detail);
        }

        // Experiment (AvatarMoveMode=mirror): the proxy's controller serves the local player's
        // captured IMovementPlayerControls data, so the engine moves and animates it as if it were
        // the player (RE A.10). No teleporting in this mode.
        if ((_opt.mirror || _opt.controls) && avatar.refId != 0 && !avatar.motionApplied &&
            avatar.loaded3DAt != kNever &&
            now - avatar.loaded3DAt >= std::chrono::seconds(1)) {
            avatar.motionApplied = true;
            // Replay object first: the player-controls agent that state 1 creates registers the actor
            // with whatever IMovementPlayerControls is installed. With ours that is a no-op; with
            // PlayerControls::Manager it left a dangling entry that crashed the game after the proxy
            // was deleted (see movecontrols::PurgeFromPlayerControls).
            std::string detail;
            const bool ok = _opt.controls ? movecontrols::AttachReplay(avatar.refId, id, detail)
                                          : movecontrols::AttachMirror(avatar.refId, detail);
            std::string state;
            const bool entered = movecontrols::EnterPlayerControlsState(avatar.refId, state);
            log::Info("[avatar] player {} '{}': {} controls {} ({}); player-controls state {} ({}); registry {}", id,
                      remote.name, _opt.controls ? "replay" : "mirror", ok ? "attached" : "NOT attached", detail,
                      entered ? "entered" : "NOT entered", state, movecontrols::DescribeRegistry(avatar.refId));
        }

        if ((_opt.mirror || _opt.controls) && avatar.refId != 0) {
            if (_opt.controls) DriveControls(id, remote);
            if (_opt.controls && avatar.motionApplied) ApplyWeapon(id, remote);
            // The proxy is an NPC: when attacked, its own combat AI fought back and fired on its
            // own (2026-11-03). Its actions come only from the remote player's replayed input, so
            // any combat it enters is stopped (combat controller at Actor+0x250, RE A.12).
            if (_opt.controls && avatar.motionApplied && now >= avatar.nextStopCombat) {
                if (auto* form = RE::TESForm::LookupByID(avatar.refId);
                    form && *reinterpret_cast<void* const*>(reinterpret_cast<std::byte*>(form) + 0x250)) {
                    avatar.nextStopCombat = now + std::chrono::milliseconds(500);
                    avatars::StopCombat(avatar.refId);
                    if (++avatar.combatStops <= 10) {
                        log::Info("[avatar] player {} '{}': proxy entered combat on its own; stopped (#{})", id,
                                  remote.name, avatar.combatStops);
                    }
                }
            }
            if (_opt.controls && avatar.motionApplied) {
                for (const auto& name : remote.pendingReplays) {
                    if (!Replayed(name)) continue;  // only allow-listed events are ever sent into a graph
                    const bool ok = animevents::Notify(avatar.refId, name.c_str());
                    // The graph event only animates; the lift comes from the engine's actor jump,
                    // which its JumpAnimEventHandler performs for the player (RE A.12).
                    if (name == "JumpStart") {
                        std::string why;
                        const bool jumped = movecontrols::Jump(avatar.refId, &why);
                        if (_opt.animDiag >= 1) {
                            log::Info("[anim-replay] proxy {:08X}: engine jump {} ({})", avatar.refId,
                                      jumped ? "started" : "refused", why);
                        }
                    }
                    if (_opt.animDiag >= 1) {
                        log::Info("[anim-replay] proxy {:08X} ({}): {}{}", avatar.refId, remote.name, name,
                                  ok ? "" : " (refused by graph)");
                    }
                }
            }
            remote.pendingReplays.clear();
            // Engine-driven only. If the controller leaves movement state 1, re-enter it at most
            // every 2 s and count it.
            if (avatar.motionApplied && now >= avatar.nextStateFix &&
                movecontrols::ControllerState(avatar.refId) != 1) {
                avatar.nextStateFix = now;  // check every frame; the AI can reset the state at any time
                std::string state;
                movecontrols::EnterPlayerControlsState(avatar.refId, state);
                if (++avatar.stateFixes <= 10) {
                    log::Info("[avatar] player {} '{}': controller left state 1; re-entered ({}), #{}", id,
                              remote.name, state, avatar.stateFixes);
                }
            }
        } else if (_opt.path && avatar.refId != 0) {
            DrivePath(id, remote);
        } else if (_opt.move && avatar.refId != 0 && now >= avatar.nextMove) {
            avatar.nextMove = now + kAvatarMoveInterval;
            if (_opt.animate) DriveLocomotion(avatar, *remote.pose);
            if (!avatars::Move(avatar.refId, remote.pose->position, remote.pose->rotation)) {
                log::Warn("[avatar] player {} '{}': ref {:08X} no longer exists", id, remote.name, avatar.refId);
                avatar.refId = 0;
            }
        }
    }

    // AvatarMoveMode=controls: serve the remote player's newest engine movement input to the proxy's
    // replay object, and correct drift between the engine-moved proxy and the authoritative pose.
    void GameBridge::DriveControls(PlayerId id, RemotePlayer& remote)
    {
        constexpr auto kControlsTimeout = std::chrono::milliseconds(300);  // then stand still
        constexpr auto kSnapCooldown = std::chrono::milliseconds(1000);
        auto& a = remote.avatar;
        const auto now = std::chrono::steady_clock::now();
        const bool fresh = remote.controls && now - remote.controlsAt < kControlsTimeout;
        movecontrols::Controls c;
        if (remote.controls) {
            std::memcpy(c.data, remote.controls->values.data(), sizeof(float) * proto::kControlFloats);
            c.data[proto::kControlFloats * sizeof(float)] = static_cast<std::byte>(remote.controls->flag);
            c.seq = remote.controlsSeq;
        }
        movecontrols::SetReplay(id, c, fresh);

        // Yaw, closed-loop: turn the proxy by its heading error to the remote player's interpolated
        // yaw, through the engine's own turn input. No accumulation of the sender's deltas.
        if (a.motionApplied && remote.pose) {
            auto* form = RE::TESForm::LookupByID(a.refId);
            if (form && form->Is(RE::FormType::kACHR)) {
                const auto angle = static_cast<RE::Actor*>(form)->GetAngle();
                // Facing like the remote's visible body (2026-11-03: copying the aim yaw made the proxy
                // backpedal where the player turns around and runs toward the camera):
                //   weapon drawn      -> aim yaw (combat stance strafes and backpedals)
                //   holstered, moving -> direction of travel (the body turns into the run)
                //   standing          -> the remote's view yaw
                const bool drawn = remote.weapon && remote.weapon->drawn;
                const float rs = std::sqrt(remote.velX * remote.velX + remote.velY * remote.velY);
                float targetYaw = remote.haveLatest ? remote.latestYaw : remote.pose->rotation.z;
                if (drawn) {
                    a.bodyYaw = targetYaw;
                } else if (rs > 0.5f) {
                    a.bodyYaw = std::atan2(remote.velX, remote.velY);  // forward = (sin yaw, cos yaw)
                    targetYaw = a.bodyYaw;
                } else {
                    a.bodyYaw = targetYaw;  // standing: turn with the remote's view (in-game: idle turning expected)
                }
                a.haveBodyYaw = true;
                const float err = TransformInterpolator::AngleDelta(angle.z, targetYaw);
                // +0x18 behaves as a turn RATE, not a per-frame angle: the player's own input reads
                // about +-1.8 during fast mouse turns (2026-11-03), and a small per-frame step made the
                // proxy turn "really really slow". Proportional rate, capped at the player's range.
                constexpr float kTurnGain = 10.0f;  // 1/s
                constexpr float kMaxTurnRate = 3.0f;
                a.lastTurnAt = now;
                const float delta = std::clamp(err * kTurnGain, -kMaxTurnRate, kMaxTurnRate);
                movecontrols::SetReplayTurn(id, delta, angle.z + err);
                if (_opt.animDiag >= 2 && now >= a.nextLookLog) {
                    a.nextLookLog = now + std::chrono::milliseconds(500);
                    log::Info("[look] player {}: proxy yaw {:.2f} remote yaw {:.2f} err {:+.2f} | pitch proxy {:.2f} "
                              "remote {:.2f}",
                              id, angle.z, targetYaw, err, angle.x, remote.pose->rotation.x);
                }
            }
        }

        if (!a.motionApplied || !remote.haveLatest) return;

        // Look pitch: the proxy graph's fCameraPitch (unused by NPC code, so the value sticks; the
        // player's graph reads its own, RE A.13). Papyrus write, at most 30 Hz, only on change.
        if (now >= a.nextPitchWrite && std::abs(remote.latestPitch - a.sentPitch) > 0.003f) {
            a.nextPitchWrite = now + std::chrono::milliseconds(33);
            if (avatars::SetAnimFloat(a.refId, "fCameraPitch", remote.latestPitch)) a.sentPitch = remote.latestPitch;
        }

        // NPC aim path (RE A.4/A.13): write the remote look pitch into the proxy's bound DirectAt
        // fields every frame (its AI leaves them at 0 while it is not fighting). Sign convention
        // relative to fCameraPitch: +1 assumed, to be confirmed in game.
        {
            constexpr float kAimPitchSign = 1.0f;
            const bool drawn = remote.weapon && remote.weapon->drawn;
            movecontrols::SetAim(a.refId, kAimPitchSign * remote.latestPitch, drawn);
        }

        // Position: compare with the newest received position extrapolated by the remote velocity
        // (not the 100 ms-delayed interpolation, which pulled a moving proxy backwards: 2026-11-03).
        const auto state = avatars::Probe(a.refId);
        if (!state.exists) return;
        const float age = std::clamp(static_cast<float>(net::SteadyNowMs() - remote.latestReceivedMs) / 1000.0f, 0.0f, 0.25f);
        const Vec3 target{ remote.latestPos.x + remote.velX * age, remote.latestPos.y + remote.velY * age,
                           remote.latestPos.z + remote.velZ * age };
        const float dx = target.x - state.position.x, dy = target.y - state.position.y, dz = target.z - state.position.z;
        const float gap = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float remoteSpeed = std::sqrt(remote.velX * remote.velX + remote.velY * remote.velY);

        // Closed-loop movement: desired velocity = remote velocity + Kp * position error, turned into
        // the engine's own move input (direction relative to facing + magnitude). The engine then
        // moves and animates the proxy at that speed: no open-loop drift, and slow movement walks.
        {
            constexpr float kGain = 2.0f;           // 1/s: closes a 1-unit error at 2 u/s extra
            constexpr float kRunSpeed = 5.5f;       // u/s at magnitude 2.0 (measured: Speed 5.49)
            constexpr float kMaxMagnitude = 2.0f;
            float vx = remote.velX + kGain * dx, vy = remote.velY + kGain * dy;
            float speed = std::sqrt(vx * vx + vy * vy);
            const float planarGap = std::sqrt(dx * dx + dy * dy);
            float angle = 0.0f, magnitude = 0.0f;
            if (!(remoteSpeed < 0.2f && planarGap < 0.25f) && speed > 0.05f) {
                auto* form = RE::TESForm::LookupByID(a.refId);
                const float yaw = form ? static_cast<RE::TESObjectREFR*>(form)->GetAngle().z : 0.0f;
                // Engine convention: forward = (sin yaw, cos yaw), right = (cos yaw, -sin yaw).
                const float fwd = vx * std::sin(yaw) + vy * std::cos(yaw);
                const float right = vx * std::cos(yaw) - vy * std::sin(yaw);
                angle = std::atan2(right, fwd);
                magnitude = std::clamp(speed / kRunSpeed * 2.0f, 0.0f, kMaxMagnitude);
            }
            movecontrols::SetReplayMove(id, angle, magnitude);
            if (_opt.animDiag >= 2 && now >= a.nextMoveLog) {
                a.nextMoveLog = now + std::chrono::milliseconds(500);
                log::Info("[move] player {}: gap {:.2f} remote {:.2f} u/s -> desired {:.2f} u/s angle {:+.2f} mag {:.2f}",
                          id, gap, remoteSpeed, speed, angle, magnitude);
            }
        }
        constexpr std::chrono::steady_clock::time_point kNever{};
        // Large error that persists: snap.
        if (gap > 3.0f) {
            if (a.bigErrorSince == kNever) a.bigErrorSince = now;
        } else {
            a.bigErrorSince = kNever;
        }
        // Small error while the remote stands still: settle onto the exact spot (not noticeable).
        if (gap > 0.5f && remoteSpeed < 0.3f) {
            if (a.smallErrorSince == kNever) a.smallErrorSince = now;
        } else {
            a.smallErrorSince = kNever;
        }
        const bool snapBig = a.bigErrorSince != kNever && now - a.bigErrorSince > std::chrono::milliseconds(400);
        const bool settle = a.smallErrorSince != kNever && now - a.smallErrorSince > std::chrono::milliseconds(500);
        if ((snapBig || settle) && now >= a.nextSnapAt) {
            a.nextSnapAt = now + kSnapCooldown;
            a.bigErrorSince = a.smallErrorSince = kNever;
            Vec3 rot{ 0.0f, 0.0f, remote.latestYaw };
            avatars::MoveRef(a.refId, target, rot);
            if (++a.snaps <= 20 || a.snaps % 50 == 0) {
                log::Info("[avatar] player {} '{}': proxy {} {:.2f} from the remote position (#{}, remote {:.2f} u/s)",
                          id, remote.name, snapBig ? "snapped" : "settled", gap, a.snaps, remoteSpeed);
            }
        }
    }

    void GameBridge::ApplyWeapon(PlayerId id, RemotePlayer& remote)
    {
        auto& a = remote.avatar;
        const auto now = std::chrono::steady_clock::now();
        if (!remote.weapon || now < a.nextWeaponApply) return;
        const auto& w = *remote.weapon;
        if (w.formId != a.appliedWeapon) {
            a.nextWeaponApply = now + std::chrono::milliseconds(500);
            bool ok = true;
            if (a.appliedWeapon != 0) avatars::UnequipWeapon(a.refId, a.appliedWeapon);
            if (w.formId != 0) ok = avatars::EquipWeapon(a.refId, w.formId);
            log::Info("[weapon] player {} '{}': proxy weapon {:08X} -> {:08X}{}", id, remote.name, a.appliedWeapon,
                      w.formId, ok ? "" : " (not a weapon here / refused)");
            a.appliedWeapon = w.formId;
            a.appliedDrawn = -1;  // re-apply the drawn state after the equip
            return;
        }
        const int drawn = w.drawn ? 1 : 0;
        // Read the proxy's actual drawn state once a second; if its AI changed it, re-apply.
        if (a.proxyWeaponQuery && a.proxyWeaponQuery->pending.load() <= 0) {
            const int actual = a.proxyWeaponQuery->drawn.load();
            a.proxyWeaponQuery.reset();
            if (actual >= 0 && actual != drawn && a.appliedDrawn == drawn) {
                a.appliedDrawn = -1;  // force re-apply below
                if (++a.weaponReasserts <= 20) {
                    log::Info("[weapon] player {} '{}': proxy weapon was {} by its AI; re-applying (#{})", id,
                              remote.name, actual ? "drawn" : "put away", a.weaponReasserts);
                }
            }
        }
        if (!a.proxyWeaponQuery && now >= a.nextProxyWeaponCheck) {
            a.nextProxyWeaponCheck = now + std::chrono::seconds(1);
            auto q = std::make_shared<avatars::WeaponQuery>();
            if (avatars::RequestWeaponState(a.refId, q)) a.proxyWeaponQuery = std::move(q);
        }
        if (drawn != a.appliedDrawn) {
            a.nextWeaponApply = now + std::chrono::milliseconds(500);
            avatars::SetWeaponDrawn(a.refId, w.drawn);
            log::Info("[weapon] player {} '{}': proxy weapon {}", id, remote.name, w.drawn ? "drawn" : "holstered");
            a.appliedDrawn = drawn;
        }
    }

    void GameBridge::ReleaseAvatar(PlayerId id, Avatar& avatar, const char* why)
    {
        movecontrols::SetReplay(id, {}, false);
        if (avatar.pending) {
            _orphans.push_back(std::move(avatar.pending));
        }
        if (avatar.markerPending) _orphans.push_back(std::move(avatar.markerPending));
        if (avatar.markerRef != 0) {
            avatars::Despawn(avatar.markerRef);
            avatar.markerRef = 0;
        }
        avatar.pathDone.reset();
        avatar.pathWalkRun = -1.0f;
        if (avatar.refId != 0) {
            if (const int purged = movecontrols::PurgeFromPlayerControls(avatar.refId); purged > 0) {
                log::Info("[avatar] player {}: removed {} PlayerControls entr{} for ref {:08X}", id, purged,
                          purged == 1 ? "y" : "ies", avatar.refId);
            }
            log::Info("[avatar] player {}: despawn ref {:08X} ({})", id, avatar.refId, why);
            avatars::Despawn(avatar.refId);
            _despawnChecks.emplace_back(avatar.refId, std::chrono::steady_clock::now() + std::chrono::seconds(2));
            avatar.refId = 0;
        }
        avatar.attempts = 0;  // allow a fresh spawn next time they share a cell
    }

    void GameBridge::RemoveRemote(PlayerId id)
    {
        if (auto it = _remotes.find(id); it != _remotes.end()) {
            ReleaseAvatar(id, it->second.avatar, "player left");
            _remotes.erase(it);
        }
    }

    void GameBridge::RemoveAllRemotes()
    {
        for (auto& [id, remote] : _remotes) {
            ReleaseAvatar(id, remote.avatar, "session ended");
        }
        _remotes.clear();
    }

    void GameBridge::ReapOrphans()
    {
        std::erase_if(_orphans, [](const std::shared_ptr<std::atomic<std::uint32_t>>& slot) {
            const auto result = slot->load();
            if (result == 0) return false;  // still in flight
            if (!avatars::IsSpawnError(result)) {
                log::Info("[avatar] despawn late-arriving ref {:08X}", result);
                avatars::Despawn(result);
            }
            return true;
        });
    }

    // Once per frame: take the player's captured graph events (diagnostics use them too) and queue
    // the replayable inputs for the network; each is sent in 3 consecutive transforms.
    void GameBridge::DrainAnimEvents()
    {
        _frameEvents.clear();
        animevents::Drain(_frameEvents, _frameEventsDropped);
        if (!_opt.controls) return;
        for (const auto& e : _frameEvents) {
            if (!e.input || !e.accepted || !Replayed(e.tag)) continue;
            if (_outEvents.size() >= 32) _outEvents.pop_front();
            _outEvents.push_back({ _nextEventSeq++, e.tag, 3 });
        }
    }

    // NPC sync step 1 (docs/NPC_SYNC_ARCHITECTURE.md): the engine's high-process actors, with the
    // runtime layout check. Every 5 s; the full list the first three times, then on count changes.
    void GameBridge::LogNpcEnumeration()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!_inWorld || now < _nextNpcLog) return;
        _nextNpcLog = now + std::chrono::seconds(5);
        const auto e = npcsync::EnumerateHighActors();
        const bool full = _npcLogs < 3 || e.actors.size() != _lastNpcCount || !e.ok;
        _lastNpcCount = e.actors.size();
        if (!full) return;
        ++_npcLogs;
        log::Info("[npc] {} high-process actors besides the player; {}", e.actors.size(), e.check);
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto pp = player ? player->GetPosition() : RE::NiPoint3{};
        std::size_t shown = 0;
        for (const auto& a : e.actors) {
            if (++shown > 25) {
                log::Info("[npc]   ... {} more", e.actors.size() - 25);
                break;
            }
            const float dx = a.position.x - pp.x, dy = a.position.y - pp.y, dz = a.position.z - pp.z;
            log::Info("[npc]   {:08X}{} base {:08X} dist {:.1f} pos ({:.1f}, {:.1f}, {:.1f}) yaw {:.2f}", a.formId,
                      (a.formId >> 24) == 0xFF ? " (runtime)" : "", a.baseId, std::sqrt(dx * dx + dy * dy + dz * dz),
                      a.position.x, a.position.y, a.position.z, a.yaw);
        }
    }

    void GameBridge::UpdateNpcCombat(net::NetworkWorker& worker)
    {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(_combatKnown, [&](const auto& kv) { return kv.second < now; });
        const PlayerId self = worker.Status().localPlayer;
        // Completed GetCombatTarget queries: map the target to a player and report new pairs.
        for (auto it = _combatQueries.begin(); it != _combatQueries.end();) {
            const auto target = it->second->load();
            if (target == 0) {
                ++it;
                continue;
            }
            const std::uint32_t npc = it->first;
            it = _combatQueries.erase(it);
            PlayerId targetPlayer = kInvalidPlayerId;
            if (target == avatars::PlayerRefId()) {
                targetPlayer = self;
            } else {
                for (const auto& [pid, r] : _remotes) {
                    if (r.avatar.refId == target) targetPlayer = pid;
                }
            }
            if (targetPlayer == kInvalidPlayerId) continue;  // NPC vs NPC: each machine's own AI
            const auto key = std::make_pair(npc, targetPlayer);
            if (_combatKnown.contains(key)) continue;  // already reported or applied (no echo)
            _combatKnown[key] = now + std::chrono::seconds(30);
            worker.SubmitNpcCombat(npc, targetPlayer);
            log::Info("[combat] NPC {:08X} entered combat against player {}; reported", npc, targetPlayer);
        }
        if (!_inWorld || !_vmSettled || now < _nextCombatScan) return;
        _nextCombatScan = now + std::chrono::milliseconds(250);
        // Candidates: data-file NPCs (same FormID everywhere) that have a combat controller
        // (Actor+0x250, read by the engine's own NPC checks, RE A.12). Proxies excluded.
        const auto e = npcsync::EnumerateHighActors();
        if (!e.ok) return;
        for (const auto& a : e.actors) {
            if ((a.formId >> 24) == 0xFF || _combatQueries.size() >= 16 || _combatQueries.contains(a.formId)) continue;
            auto* form = RE::TESForm::LookupByID(a.formId);
            if (!form) continue;
            const auto combat = *reinterpret_cast<void* const*>(reinterpret_cast<std::byte*>(form) + 0x250);
            if (!combat) continue;
            auto slot = std::make_shared<std::atomic<std::uint32_t>>(0);
            if (avatars::RequestCombatTarget(a.formId, slot)) _combatQueries.emplace(a.formId, std::move(slot));
        }
    }

    void GameBridge::ApplyNpcCombat(const proto::NpcCombat& c, net::NetworkWorker& worker)
    {
        const PlayerId self = worker.Status().localPlayer;
        std::uint32_t targetRef = 0;
        if (c.targetPlayer == self) {
            targetRef = avatars::PlayerRefId();
        } else if (auto it = _remotes.find(c.targetPlayer); it != _remotes.end()) {
            targetRef = it->second.avatar.refId;
        }
        _combatKnown[{ c.npcFormId, c.targetPlayer }] = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        if (targetRef == 0) {
            log::Info("[combat] player {} reports NPC {:08X} fighting player {}; no local target (not spawned here)",
                      c.playerId, c.npcFormId, c.targetPlayer);
            return;
        }
        const bool ok = avatars::StartCombat(c.npcFormId, targetRef);
        log::Info("[combat] player {} reports NPC {:08X} fighting player {}: StartCombat(-> {:08X}) {}", c.playerId,
                  c.npcFormId, c.targetPlayer, targetRef, ok ? "dispatched" : "failed (NPC not loaded here?)");
    }

    void GameBridge::UpdateAnimDiag()
    {
        constexpr std::chrono::steady_clock::time_point kNever{};
        const auto now = std::chrono::steady_clock::now();
        auto* player = RE::PlayerCharacter::GetSingleton();

        // Measured horizontal speed of the local player (engine units per second), for calibration.
        if (player && _inWorld) {
            const auto p = player->GetPosition();
            const float dt = std::chrono::duration<float>(now - _lastPlayerPosAt).count();
            if (_lastPlayerPosAt == kNever || dt >= 0.2f) {
                if (_lastPlayerPosAt != kNever) {
                    const float dx = p.x - _lastPlayerPos.x, dy = p.y - _lastPlayerPos.y;
                    _playerSpeed = std::sqrt(dx * dx + dy * dy) / dt;
                }
                _lastPlayerPos = { p.x, p.y, p.z };
                _lastPlayerPosAt = now;
            }
        }

        // Animation graph events captured on the local player (AnimEvents hook), in order.
        {
            const auto& events = _frameEvents;
            const std::uint64_t dropped = _frameEventsDropped;
            std::size_t logged = 0;
            for (const auto& e : events) {
                if (++logged > 40) {
                    log::Info("[anim-event] player: ... {} more this frame", events.size() - 40);
                    break;
                }
                if (e.input) {
                    log::Info("[anim-in] player: {}{} (speed {:.2f})", e.tag, e.accepted ? "" : " (refused)",
                              _playerSpeed);
                    continue;
                }
                log::Info("[anim-event] player: {}{}{} (speed {:.2f})", e.tag, e.payload.empty() ? "" : " | ", e.payload,
                          _playerSpeed);
            }
            if (dropped != _animEventsDropped) {
                log::Warn("[anim-event] {} events dropped (buffer full)", dropped - _animEventsDropped);
                _animEventsDropped = dropped;
            }
        }

        // Native animation probe (main thread, same frame as the engine's own update), 20 Hz.
        // Discrete changes (controller mode/config, int/bool graph variables) are logged as they
        // happen; floats only at level 2, at most twice per second and only when they moved.
        if (now < _nextAnimDiag || !_inWorld) return;
        _nextAnimDiag = now + std::chrono::milliseconds(50);
        auto probeOne = [&](std::uint32_t refId, std::string_view who, animprobe::Snapshot& last, bool& probed,
                            std::chrono::steady_clock::time_point& nextFloatLog) {
            auto snap = animprobe::Read(refId);
            if (!probed) {
                log::Info("[anim-state] {}: {}", who, animprobe::DescribeState(snap));
                log::Info("[anim-state] {}: {}", who, movecontrols::DescribeRegistry(refId));
                log::Info("[anim-state] {}: {}", who, animprobe::DescribeFloats(snap));
            } else if (auto t = animprobe::DescribeTransitions(last, snap); !t.empty()) {
                log::Info("[anim-state] {}: {} (speed {:.2f})", who, t, _playerSpeed);
            }
            if (_opt.animDiag >= 2 && now >= nextFloatLog && animprobe::FloatsChanged(last, snap, 0.05f)) {
                nextFloatLog = now + std::chrono::milliseconds(500);
                log::Info("[anim-float] {}: {}", who, animprobe::DescribeFloats(snap));
            }
            probed = true;
            last = std::move(snap);
        };
        if (player) {
            probeOne(avatars::PlayerRefId(), "player", _playerProbe, _playerProbed, _nextPlayerFloatLog);
            if (!_probeSelfChecked && _playerProbe.graphPresent) {
                _probeSelfChecked = true;
                bool passed = false;
                const auto verdict = animprobe::SelfCheckPlayer(_playerProbe, passed);
                if (passed) log::Info("[anim-state] {}", verdict);
                else log::Warn("[anim-state] {}", verdict);
            }
        }
        if ((_opt.mirror || _opt.controls) && _opt.animDiag >= 2 && now >= _nextControlsLog) {
            _nextControlsLog = now + std::chrono::milliseconds(250);
            const auto c = movecontrols::LatestPlayerControls();
            if (c.seq != _loggedControlsSeq) {
                log::Info("[controls] player: {}", movecontrols::Describe(c));
                _loggedControlsSeq = c.seq;
            }
        }
        for (auto& [id, remote] : _remotes) {
            auto& a = remote.avatar;
            if (a.refId == 0 || a.loaded3DAt == kNever) continue;
            probeOne(a.refId, std::format("proxy {:08X} ({})", a.refId, remote.name), a.probe, a.probed,
                     a.nextFloatLog);
        }
    }

    // Locomotion from network motion. Calibrated on the local player (2026-11-02, AvatarAnimDiag):
    //   "Speed"     = horizontal speed in world units per second (strafe 5.0 at a measured 5.2 u/s,
    //                 backwards 4.4 at 4.4 u/s, 0 when idle)
    //   "Direction" = movement direction relative to facing as a fraction of a turn
    //                 (0 forward, 0.25 right, 0.5 back; keeps its last value when idle)
    // Engine yaw convention: forward = (sin yaw, cos yaw), so right = (cos yaw, -sin yaw).
    void GameBridge::DriveLocomotion(Avatar& avatar, const InterpolatedPose& pose)
    {
        constexpr std::chrono::steady_clock::time_point kNever{};
        constexpr float kTwoPi = 6.28318530718f;
        constexpr float kIdleSpeed = 0.3f;        // below this the proxy stands
        constexpr float kSmoothing = 0.5f;        // per 100 ms tick
        const auto now = std::chrono::steady_clock::now();
        if (avatar.loaded3DAt == kNever) return;  // no graph before 3D

        float direction = avatar.sentDirection < 0.0f ? 0.0f : avatar.sentDirection;
        if (avatar.lastPosAt != kNever) {
            const float dt = std::chrono::duration<float>(now - avatar.lastPosAt).count();
            if (dt > 0.0f && dt < 1.0f) {
                const float vx = (pose.position.x - avatar.lastPos.x) / dt;
                const float vy = (pose.position.y - avatar.lastPos.y) / dt;
                const float raw = std::sqrt(vx * vx + vy * vy);
                avatar.speed += (raw - avatar.speed) * kSmoothing;
                if (raw > kIdleSpeed) {
                    float rel = std::atan2(vx, vy) - pose.rotation.z;
                    rel = std::fmod(rel, kTwoPi);
                    if (rel < 0.0f) rel += kTwoPi;
                    direction = rel / kTwoPi;
                    if (direction >= 1.0f) direction = 0.0f;
                }
            }
        }
        avatar.lastPos = pose.position;
        avatar.lastPosAt = now;

        const float speed = avatar.speed < kIdleSpeed ? 0.0f : avatar.speed;
        // Write when the value changed meaningfully, and at least every 500 ms (the engine may reset it).
        const bool speedChanged = std::abs(speed - avatar.sentSpeed) > 0.1f;
        float dd = std::abs(direction - avatar.sentDirection);
        dd = std::min(dd, 1.0f - dd);  // wrap-around distance
        const bool dirChanged = speed > 0.0f && dd > 0.02f;
        if (!speedChanged && !dirChanged && now < avatar.nextAnimWrite) return;
        avatar.nextAnimWrite = now + std::chrono::milliseconds(500);
        avatars::SetAnimFloat(avatar.refId, "Speed", speed);
        avatar.sentSpeed = speed;
        if (speed > 0.0f || avatar.sentDirection < 0.0f) {
            avatars::SetAnimFloat(avatar.refId, "Direction", direction);
            avatar.sentDirection = direction;
        }
    }

    // Engine-driven movement: keep a marker on the remote pose and let the proxy path to it.
    void GameBridge::DrivePath(PlayerId id, RemotePlayer& remote)
    {
        constexpr std::chrono::steady_clock::time_point kNever{};
        constexpr float kRunSpeed = 5.0f;        // world units/s at normalized speed 1 (calibrated jog 5.0)
        constexpr float kArrive = 0.5f;          // target radius: stand and face the remote yaw
        constexpr float kRepathMove = 0.75f;     // re-path when the target moved this far since the last call
        constexpr float kTeleport = 8.0f;        // too far behind (load, teleport): snap
        constexpr float kLeadSeconds = 0.25f;    // aim ahead of the (delayed) interpolated pose
        constexpr auto kRepathInterval = std::chrono::milliseconds(250);
        constexpr auto kSnapCooldown = std::chrono::milliseconds(1500);
        auto& a = remote.avatar;
        const auto& pose = *remote.pose;
        const auto now = std::chrono::steady_clock::now();
        if (a.loaded3DAt == kNever) return;

        // 1. Marker.
        if (a.markerRef == 0) {
            if (a.markerPending) {
                const auto r = a.markerPending->load();
                if (r == 0) return;
                a.markerPending.reset();
                if (avatars::IsSpawnError(r)) {
                    log::Warn("[avatar] player {}: marker spawn failed: {}; using teleport", id,
                              avatars::DescribeSpawnError(r));
                    _opt.path = false;
                    return;
                }
                a.markerRef = r;
                log::Info("[avatar] player {}: path marker {:08X}", id, r);
            } else {
                auto slot = std::make_shared<std::atomic<std::uint32_t>>(0);
                std::string error;
                if (avatars::RequestMarker(slot, error)) {
                    a.markerPending = std::move(slot);
                } else {
                    log::Warn("[avatar] player {}: marker not dispatched: {}; using teleport", id, error);
                    _opt.path = false;
                }
                return;
            }
        }

        // 2. Remote velocity from the pose stream (smoothed).
        if (a.lastPosAt != kNever) {
            const float dt = std::chrono::duration<float>(now - a.lastPosAt).count();
            if (dt >= 0.05f) {
                const float vx = (pose.position.x - a.lastPos.x) / dt, vy = (pose.position.y - a.lastPos.y) / dt;
                a.velX += (vx - a.velX) * 0.5f;
                a.velY += (vy - a.velY) * 0.5f;
                a.speed = std::sqrt(a.velX * a.velX + a.velY * a.velY);
                a.lastPos = pose.position;
                a.lastPosAt = now;
            }
        } else {
            a.lastPos = pose.position;
            a.lastPosAt = now;
        }
        const bool moving = a.speed > 0.3f;
        Vec3 target = pose.position;
        if (moving) {
            target.x += a.velX * kLeadSeconds;
            target.y += a.velY * kLeadSeconds;
        }

        // 3. Keep the marker on the (led) remote pose, 20 Hz.
        if (now >= a.nextMove) {
            a.nextMove = now + std::chrono::milliseconds(50);
            avatars::MoveRef(a.markerRef, target, pose.rotation);
        }

        // 4. Where is the proxy?
        const auto state = avatars::Probe(a.refId);
        if (!state.exists) return;
        const float gx = pose.position.x - state.position.x, gy = pose.position.y - state.position.y;
        const float gap = std::sqrt(gx * gx + gy * gy);

        if (_opt.animDiag && now >= a.nextAnimWrite) {
            a.nextAnimWrite = now + std::chrono::seconds(1);
            log::Info("[path] player {}: gap {:.2f} remote {:.2f} u/s path speed {:.2f} calls {}", id, gap, a.speed,
                      a.pathWalkRun, a.pathCalls);
        }
        if (gap > kTeleport && now >= a.nextSnapAt) {
            avatars::MoveRef(a.refId, pose.position, pose.rotation);
            a.pathDone.reset();
            a.nextSnapAt = now + kSnapCooldown;
            log::Info("[avatar] player {}: {:.1f} units behind; snapped", id, gap);
            return;
        }
        if (!moving && gap < kArrive) {
            if (!a.pathIdleSnapped) {  // face where the remote faces once it stops
                avatars::MoveRef(a.refId, state.position, pose.rotation);
                a.pathIdleSnapped = true;
            }
            return;
        }
        a.pathIdleSnapped = false;

        // 5. Speed: the remote's own speed plus catch-up proportional to the gap.
        const float speed = std::clamp(a.speed / kRunSpeed + gap * 0.15f, 0.25f, 1.0f);

        // Log PathToReference results when they change (2026-11-02: the proxy stalled while calls
        // returned at ~10/s, so the returned value is needed to tell refusal from arrival).
        if (a.pathDone) {
            const auto v = a.pathDone->load();
            if (v != 0 && v != a.lastPathResult) {
                a.lastPathResult = v;
                log::Info("[path] player {}: PathToReference returned {} after {} ms (gap {:.2f})", id,
                          v == 0xFFFFFFF0u ? std::string("<cancelled>")
                          : v == 0xFFFFFFF1u ? std::string("-1")
                                             : std::to_string(static_cast<std::int64_t>(v) - 1),
                          std::chrono::duration_cast<std::chrono::milliseconds>(now - a.pathStartedAt).count(), gap);
            }
        }

        // 6. Re-path when idle, or every 250 ms if the target moved or the speed changed.
        const bool inFlight = a.pathDone && a.pathDone->load() == 0;
        const float mx = target.x - a.pathTarget.x, my = target.y - a.pathTarget.y;
        const bool targetMoved = std::sqrt(mx * mx + my * my) > kRepathMove;
        const bool speedChanged = std::abs(speed - a.pathWalkRun) > 0.2f;
        if (inFlight && (now - a.pathStartedAt < kRepathInterval || (!targetMoved && !speedChanged))) return;
        if (!inFlight && a.pathDone && now - a.pathStartedAt < std::chrono::milliseconds(100)) return;
        auto done = std::make_shared<std::atomic<std::uint32_t>>(0);
        if (avatars::RequestPathTo(a.refId, a.markerRef, speed, kArrive, done)) {
            a.pathDone = std::move(done);
            a.pathStartedAt = now;
            a.pathWalkRun = speed;
            a.pathTarget = target;
            ++a.pathCalls;
        }
    }

    void GameBridge::CheckDespawns()
    {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(_despawnChecks, [now](const auto& entry) {
            if (now < entry.second) return false;
            log::Info("[avatar] despawned ref {:08X} 2 s later: {}", entry.first,
                      avatars::Describe(avatars::Probe(entry.first)));
            return true;
        });
    }

    void GameBridge::LogSummary(net::NetworkWorker& worker)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < _nextSummary) {
            return;
        }
        _nextSummary = now + kSummaryInterval;

        const auto status = worker.Status();
        log::Info("[game] status: {} as player {}, {} remote players, rtt {} ms, {} dropped events",
                  net::ToString(status.state), status.localPlayer, _remotes.size(), status.rttMs, status.droppedEvents);
        for (const auto& [id, r] : _remotes) {
            if (!r.pose) {
                log::Info("[game]   {} '{}': no position yet", id, r.name);
                continue;
            }
            const bool sameLocation = r.pose->locationId == _localLocationId && _localLocationId != 0;
            log::Info("[game]   {} '{}': {} samples, pose ({:.1f}, {:.1f}, {:.1f}) yaw {:.2f}{}{} [{}]", id, r.name,
                      r.samplesReceived, r.pose->position.x, r.pose->position.y, r.pose->position.z,
                      r.pose->rotation.z, r.pose->extrapolated ? " extrapolated" : "", r.pose->stale ? " stale" : "",
                      sameLocation ? "same location as you" : "elsewhere");
        }
    }

    void GameBridge::RebuildFromRoster(net::NetworkWorker& worker)
    {
        const auto roster = worker.Roster();
        for (auto& [id, remote] : _remotes) {
            if (!roster.contains(id)) ReleaseAvatar(id, remote.avatar, "player left");
        }
        std::map<PlayerId, RemotePlayer> next;
        for (const auto& [id, name] : roster) {
            if (auto it = _remotes.find(id); it != _remotes.end()) {
                next.emplace(id, std::move(it->second));
            } else {
                next[id].name = name;
            }
        }
        _remotes = std::move(next);
    }
}
