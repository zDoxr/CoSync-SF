#include "GameBridge.h"

#include "GameState.h"

#include "cosync/net/Util.h"
#include "cosync/shared/Log.h"

namespace cosync::plugin
{
    namespace
    {
        constexpr std::size_t kMaxEventsPerFrame = 256;  // bounds per-frame work
        constexpr auto kSampleInterval = std::chrono::milliseconds(50);  // 20 Hz
        constexpr auto kLocalLogInterval = std::chrono::seconds(5);
        constexpr auto kSummaryInterval = std::chrono::seconds(5);
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
                            _remotes.clear();  // avatar despawn point
                        }
                    } else if constexpr (std::is_same_v<T, ce::Welcomed>) {
                        log::Info("[game] joined '{}' as player {}", e.welcome.serverName, e.welcome.playerId);
                    } else if constexpr (std::is_same_v<T, ce::Rejected>) {
                        log::Warn("[game] session refused: {} ({})", proto::ToString(e.reject.reason), e.reject.detail);
                    } else if constexpr (std::is_same_v<T, ce::PeerJoined>) {
                        _remotes[e.player.playerId].name = e.player.name;  // avatar spawn point
                        log::Info("[game] player {} '{}' joined", e.player.playerId, e.player.name);
                    } else if constexpr (std::is_same_v<T, ce::PeerLeft>) {
                        _remotes.erase(e.playerId);  // avatar despawn point
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
            // Avatar update point: once spawning is verified in game, move this
            // player's proxy reference to remote.pose here (main thread).
        }
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
        std::map<PlayerId, RemotePlayer> next;
        for (const auto& [id, name] : worker.Roster()) {
            if (auto it = _remotes.find(id); it != _remotes.end()) {
                next.emplace(id, std::move(it->second));
            } else {
                next[id].name = name;
            }
        }
        _remotes = std::move(next);
    }
}
