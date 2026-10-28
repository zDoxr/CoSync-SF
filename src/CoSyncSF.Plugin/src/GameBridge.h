#pragma once

#include "cosync/net/NetworkWorker.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace cosync::plugin
{
    // Game-thread side of the plugin. Tick() runs once per frame on Starfield's
    // main thread (SFSE permanent task) and is the ONLY place that will touch
    // engine objects once game integration lands. At this milestone it consumes
    // network events and records/logs them; it makes no engine calls.
    class GameBridge
    {
    public:
        static GameBridge& Get();

        // Hand over the worker (owned elsewhere, outlives the bridge's use of it).
        void Attach(net::NetworkWorker* worker) noexcept { _worker.store(worker); }
        void Tick();

    private:
        struct RemotePlayer
        {
            std::string name;
            proto::PlayerTransform lastTransform;
            std::uint64_t transformsReceived = 0;
        };

        void RebuildFromRoster(net::NetworkWorker& worker);

        std::atomic<net::NetworkWorker*> _worker{ nullptr };
        std::uint64_t _frame = 0;
        std::vector<net::ClientEvent> _events;
        std::vector<proto::PlayerTransform> _transforms;
        std::map<PlayerId, RemotePlayer> _remotes;
    };
}
