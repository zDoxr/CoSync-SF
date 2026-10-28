#pragma once

#include "cosync/net/NetworkWorker.h"
#include "cosync/shared/Interpolation.h"

#include <atomic>
#include <memory>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace cosync::plugin
{
    // Game-thread side of the plugin. Tick() runs once per frame on Starfield's
    // main thread (SFSE permanent task) and is the ONLY caller of engine code
    // (GameState.cpp). It samples the local player's transform for the network
    // thread, consumes session events, and keeps an interpolated pose for every
    // remote player.
    class GameBridge
    {
    public:
        static GameBridge& Get();

        // Hand over the worker (owned elsewhere, outlives the bridge's use of it).
        void Attach(net::NetworkWorker* worker) noexcept { _worker.store(worker); }
        // Reading the player needs the Address Library; disabled if it is missing.
        void EnableLocalPlayerReads(bool enable) noexcept { _readLocalPlayer = enable; }
        // Experimental: spawn and move a stand-in actor per remote player in the same cell.
        void EnableAvatars(bool enable) noexcept { _avatarsEnabled = enable; }
        void Tick();

    private:
        struct Avatar
        {
            std::shared_ptr<std::atomic<std::uint32_t>> pending;  // spawn in flight
            std::uint32_t refId = 0;
            std::uint32_t attempts = 0;
            std::chrono::steady_clock::time_point nextAttempt{};
            std::chrono::steady_clock::time_point nextMove{};
        };

        struct RemotePlayer
        {
            std::string name;
            Avatar avatar;
            TransformInterpolator interpolator;
            std::optional<InterpolatedPose> pose;  // this frame's render pose
            std::uint64_t samplesReceived = 0;
        };

        void RebuildFromRoster(net::NetworkWorker& worker);
        void SampleLocalPlayer(net::NetworkWorker& worker);
        void UpdateRemotePlayers(net::NetworkWorker& worker);
        void LogSummary(net::NetworkWorker& worker);
        void UpdateAvatar(PlayerId id, RemotePlayer& remote);
        void ReleaseAvatar(PlayerId id, Avatar& avatar, const char* why);
        void RemoveRemote(PlayerId id);
        void RemoveAllRemotes();
        void ReapOrphans();

        std::atomic<net::NetworkWorker*> _worker{ nullptr };
        bool _readLocalPlayer = false;
        bool _inWorld = false;
        bool _avatarsEnabled = false;
        // Spawns that were still in flight when their avatar was released.
        std::vector<std::shared_ptr<std::atomic<std::uint32_t>>> _orphans;
        std::uint32_t _localLocationId = 0;
        std::string _localLocationName;
        std::chrono::steady_clock::time_point _nextSample{};
        std::chrono::steady_clock::time_point _nextLocalLog{};
        std::chrono::steady_clock::time_point _nextSummary{};
        std::vector<net::ClientEvent> _events;
        std::vector<net::RemoteSample> _samples;
        std::map<PlayerId, RemotePlayer> _remotes;
    };
}
