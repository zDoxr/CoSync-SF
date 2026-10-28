#pragma once

#include "cosync/net/NetworkWorker.h"

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace cosync::plugin
{
    // Game-thread side of the plugin. Tick() runs once per frame on Starfield's
    // main thread (SFSE permanent task) and is the ONLY caller of engine code
    // (GameState.cpp). It samples the local player's transform for the network
    // thread and consumes session events and remote transforms.
    class GameBridge
    {
    public:
        static GameBridge& Get();

        // Hand over the worker (owned elsewhere, outlives the bridge's use of it).
        void Attach(net::NetworkWorker* worker) noexcept { _worker.store(worker); }
        // Reading the player needs the Address Library; disabled if it is missing.
        void EnableLocalPlayerReads(bool enable) noexcept { _readLocalPlayer = enable; }
        void Tick();

    private:
        struct RemotePlayer
        {
            std::string name;
            proto::PlayerTransform lastTransform;
            std::uint64_t transformsReceived = 0;
        };

        void RebuildFromRoster(net::NetworkWorker& worker);
        void SampleLocalPlayer(net::NetworkWorker& worker);

        std::atomic<net::NetworkWorker*> _worker{ nullptr };
        std::uint64_t _frame = 0;
        bool _readLocalPlayer = false;
        std::chrono::steady_clock::time_point _nextSample{};
        std::chrono::steady_clock::time_point _nextLocalLog{};
        std::chrono::steady_clock::time_point _nextSummary{};
        bool _inWorld = false;
        std::vector<net::ClientEvent> _events;
        std::vector<proto::PlayerTransform> _transforms;
        std::map<PlayerId, RemotePlayer> _remotes;
    };
}
