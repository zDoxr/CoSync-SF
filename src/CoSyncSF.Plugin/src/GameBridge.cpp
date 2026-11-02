#include "GameBridge.h"

#include "Avatars.h"
#include "GameState.h"

#include "cosync/net/Util.h"
#include "cosync/shared/Log.h"

#include <RE/Starfield.h>

#include <algorithm>
#include <cmath>

namespace cosync::plugin
{
    namespace
    {
        constexpr std::size_t kMaxEventsPerFrame = 256;  // bounds per-frame work
        constexpr auto kSampleInterval = std::chrono::milliseconds(50);  // 20 Hz
        constexpr auto kLocalLogInterval = std::chrono::seconds(5);
        constexpr auto kSummaryInterval = std::chrono::seconds(5);
        constexpr auto kAvatarMoveInterval = std::chrono::milliseconds(100);  // Papyrus calls; 10 Hz
        constexpr auto kAvatarRetryInterval = std::chrono::seconds(5);
        constexpr auto kAvatarSpawnTimeout = std::chrono::seconds(10);  // e.g. VM frozen during a load
        constexpr std::uint32_t kAvatarLoggedAttempts = 6;              // keep retrying, but quietly
        constexpr auto kVmSettleTime = std::chrono::seconds(2);           // VM must be running this long first
        constexpr std::uint32_t kTemplateMaxTries = 16;  // FindRandomActor may return the player or leveled NPCs
    }

    GameBridge& GameBridge::Get()
    {
        static GameBridge instance;
        return instance;
    }

    void GameBridge::Tick()
    {
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
        LogSummary(*worker);
    }

    void GameBridge::SampleLocalPlayer(net::NetworkWorker& worker)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < _nextSample) {
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
        worker.SubmitLocalTransform(sample->locationId, sample->position, sample->rotation);

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
            }
        }

        // Same clock domain as RemoteSample::receivedMs (stamped on the network thread).
        const auto nowMs = net::SteadyNowMs();
        if (_avatarsEnabled) {
            UpdateVmState();
            UpdateTemplate();
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

        const bool visibleHere = _inWorld && remote.pose && !remote.pose->stale && _localLocationId != 0 &&
                                 remote.pose->locationId == _localLocationId;
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

        if (_opt.move && avatar.refId != 0 && now >= avatar.nextMove) {
            avatar.nextMove = now + kAvatarMoveInterval;
            if (_opt.animate) DriveLocomotion(avatar, *remote.pose);
            if (!avatars::Move(avatar.refId, remote.pose->position, remote.pose->rotation)) {
                log::Warn("[avatar] player {} '{}': ref {:08X} no longer exists", id, remote.name, avatar.refId);
                avatar.refId = 0;
            }
        }
    }

    void GameBridge::ReleaseAvatar(PlayerId id, Avatar& avatar, const char* why)
    {
        if (avatar.pending) {
            _orphans.push_back(std::move(avatar.pending));
        }
        if (avatar.refId != 0) {
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

    namespace
    {
        using avatars::AnimVarKind;
        // Locomotion graph variable candidates (strings found in Starfield.exe 1.16.244).
        // Locomotion graph variables that exist on humanoid graphs (runtime check 2026-11-02:
        // SpeedSampled, SpeedSmoothed, IsMoving, IsRunning, IsSprinting, LocomotionBlend,
        // LocomotionType, bInJumpState are "cannot fetch" on both player and NPC graphs).
        constexpr avatars::AnimVar kDiagVars[] = {
            { "Speed", AnimVarKind::kFloat },
            { "Direction", AnimVarKind::kFloat },
            { "TurnDelta", AnimVarKind::kFloat },
        };
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

        // Log reads that have completed.
        if (_playerAnim && _playerAnim->remaining.load() <= 0) {
            log::Info("[anim] player: measured {:.2f} u/s yaw {:.2f} | {}", _playerSpeed,
                      player ? player->GetAngle().z : 0.0f, avatars::FormatReadout(*_playerAnim, kDiagVars));
            _playerAnim.reset();
        }
        for (auto& [id, remote] : _remotes) {
            auto& a = remote.avatar;
            if (a.anim && a.anim->remaining.load() <= 0) {
                log::Info("[anim] proxy {:08X} ({}): {}", a.refId, remote.name,
                          avatars::FormatReadout(*a.anim, kDiagVars));
                a.anim.reset();
            }
        }

        if (now < _nextAnimDiag || !_vmSettled || !_inWorld) return;
        _nextAnimDiag = now + std::chrono::seconds(1);
        if (!_playerAnim) _playerAnim = avatars::RequestAnimVars(avatars::PlayerRefId(), kDiagVars);
        for (auto& [id, remote] : _remotes) {
            auto& a = remote.avatar;
            if (a.refId != 0 && !a.anim && a.loaded3DAt != kNever) {
                a.anim = avatars::RequestAnimVars(a.refId, kDiagVars);
            }
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
