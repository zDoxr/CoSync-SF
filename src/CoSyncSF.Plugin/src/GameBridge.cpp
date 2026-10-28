#include "GameBridge.h"

#include "cosync/shared/Log.h"

namespace cosync::plugin
{
    namespace
    {
        constexpr std::size_t kMaxEventsPerFrame = 256;  // bounds per-frame work
        constexpr std::uint64_t kSummaryEveryFrames = 1800;
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
        ++_frame;

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
                            _remotes.clear();  // Phase B: despawn all proxies here
                        }
                    } else if constexpr (std::is_same_v<T, ce::Welcomed>) {
                        log::Info("[game] joined '{}' as player {}", e.welcome.serverName, e.welcome.playerId);
                    } else if constexpr (std::is_same_v<T, ce::Rejected>) {
                        log::Warn("[game] session refused: {} ({})", proto::ToString(e.reject.reason), e.reject.detail);
                    } else if constexpr (std::is_same_v<T, ce::PeerJoined>) {
                        _remotes[e.player.playerId].name = e.player.name;  // Phase B: spawn proxy
                        log::Info("[game] player {} '{}' joined", e.player.playerId, e.player.name);
                    } else if constexpr (std::is_same_v<T, ce::PeerLeft>) {
                        _remotes.erase(e.playerId);  // Phase B: despawn proxy
                        log::Info("[game] player {} left ({})", e.playerId, proto::ToString(e.reason));
                    }
                },
                ev);
        }

        _transforms.clear();
        worker->TakeRemoteTransforms(_transforms);
        for (const auto& t : _transforms) {
            if (auto it = _remotes.find(t.playerId); it != _remotes.end()) {
                it->second.lastTransform = t;  // Phase B: drive proxy interpolation
                ++it->second.transformsReceived;
            }
        }

        if (_frame % kSummaryEveryFrames == 0) {
            const auto status = worker->Status();
            log::Info("[game] status: {} as player {}, {} remote players, rtt {} ms, {} dropped events",
                      net::ToString(status.state), status.localPlayer, _remotes.size(), status.rttMs,
                      status.droppedEvents);
            for (const auto& [id, r] : _remotes) {
                log::Debug("[game]   {} '{}': {} transforms, last ({:.1f}, {:.1f}, {:.1f})", id, r.name,
                           r.transformsReceived, r.lastTransform.position.x, r.lastTransform.position.y,
                           r.lastTransform.position.z);
            }
        }
    }

    void GameBridge::RebuildFromRoster(net::NetworkWorker& worker)
    {
        std::map<PlayerId, RemotePlayer> next;
        for (const auto& [id, name] : worker.Roster()) {
            auto& r = next[id];
            r.name = name;
            if (auto it = _remotes.find(id); it != _remotes.end()) {
                r.lastTransform = it->second.lastTransform;
                r.transformsReceived = it->second.transformsReceived;
            }
        }
        _remotes = std::move(next);
    }
}
