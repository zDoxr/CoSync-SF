#include "GameBridge.h"

#include "Avatars.h"
#include "GameState.h"

#include "cosync/net/Util.h"
#include "cosync/shared/Log.h"

#include <algorithm>

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
        for (auto& [id, remote] : _remotes) {
            remote.pose = remote.interpolator.Sample(nowMs);
            if (_avatarsEnabled) {
                UpdateAvatar(id, remote);
            }
        }
        ReapOrphans();
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
                    const bool frozen = avatars::Freeze(avatar.refId);
                    log::Info("[avatar] player {} '{}': spawned ref {:08X} (AI {})", id, remote.name, avatar.refId,
                              frozen ? "disable requested" : "NOT disabled");
                }
            }
        }

        const bool visibleHere = _inWorld && remote.pose && !remote.pose->stale && _localLocationId != 0 &&
                                 remote.pose->locationId == _localLocationId;
        if (!visibleHere) {
            ReleaseAvatar(id, avatar, "not in your cell");
            return;
        }

        if (avatar.refId == 0 && !avatar.pending && now >= avatar.nextAttempt) {
            ++avatar.attempts;
            // Back off: 5 s, 10 s, ... capped at 30 s.
            avatar.nextAttempt = now + std::min(kAvatarRetryInterval * avatar.attempts, std::chrono::seconds(30));
            // Alternate base forms so a refusal specific to one of them is visible.
            const auto which = (avatar.attempts % 2 == 1) ? avatars::SpawnBase::PlayerBase : avatars::SpawnBase::NearbyNpc;
            auto slot = std::make_shared<std::atomic<std::uint32_t>>(0);
            std::string error;
            std::string detail;
            if (avatars::RequestSpawn(which, slot, error, detail)) {
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

        if (avatar.refId != 0 && now >= avatar.nextMove) {
            avatar.nextMove = now + kAvatarMoveInterval;
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
