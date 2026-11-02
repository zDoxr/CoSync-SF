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
        void SetAvatarScale(float scale) noexcept { _avatarScale = scale; }
        void SetAvatarOptions(bool freezeAI, bool move) noexcept
        {
            _avatarFreezeAI = freezeAI;
            _avatarMove = move;
        }
        void Tick();

    private:
        struct Avatar
        {
            std::shared_ptr<std::atomic<std::uint32_t>> pending;  // spawn in flight
            std::uint32_t refId = 0;
            std::uint32_t attempts = 0;
            std::chrono::steady_clock::time_point nextAttempt{};
            std::chrono::steady_clock::time_point nextMove{};
            std::chrono::steady_clock::time_point requestedAt{};
            std::chrono::steady_clock::time_point nextProbe{};  // engine-state validation log
            std::uint32_t probes = 0;
            std::string baseDetail;
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
        void CheckDespawns();
        void UpdateVmState();
        void UpdateTemplate();
        [[nodiscard]] bool TemplateDecided() const noexcept;

        std::atomic<net::NetworkWorker*> _worker{ nullptr };
        bool _readLocalPlayer = false;
        bool _inWorld = false;
        bool _avatarsEnabled = false;
        float _avatarScale = 1.0f;
        bool _avatarFreezeAI = false;
        bool _avatarMove = true;
        bool _vmReady = false;
        bool _vmSettled = false;
        std::chrono::steady_clock::time_point _vmReadySince{};
        // Nearby NPC whose data-file base the avatars copy (0 = not found yet / player base).
        std::uint32_t _templateActor = 0;
        std::uint32_t _templateBase = 0;
        std::uint32_t _templateTries = 0;
        std::shared_ptr<std::atomic<std::uint32_t>> _templateQuery;
        std::chrono::steady_clock::time_point _templateQueriedAt{};
        std::chrono::steady_clock::time_point _nextTemplateTry{};
        // Spawns that were still in flight when their avatar was released.
        std::vector<std::shared_ptr<std::atomic<std::uint32_t>>> _orphans;
        // Despawned refs to re-check (Disable/Delete run asynchronously on the VM).
        std::vector<std::pair<std::uint32_t, std::chrono::steady_clock::time_point>> _despawnChecks;
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
