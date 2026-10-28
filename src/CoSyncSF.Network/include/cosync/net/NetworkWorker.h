#pragma once

#include "cosync/net/SessionClient.h"
#include "cosync/net/SessionServer.h"
#include "cosync/shared/Concurrency.h"
#include "cosync/shared/Interpolation.h"

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cosync::net
{
    enum class WorkerMode
    {
        Host,    // run a SessionServer in-process and join it as a local client
        Client,  // join a remote host or dedicated session server
    };

    struct WorkerConfig
    {
        WorkerMode mode = WorkerMode::Client;
        Endpoint connectTo;                 // Client mode
        std::uint16_t listenPort = 37015;   // Host mode
        std::uint16_t discoveryPort = 0;    // Host mode; 0 disables LAN discovery
        ServerConfig server;                // Host mode
        ClientConfig client;
        std::uint32_t tickHz = 60;
        std::size_t eventQueueCapacity = 1024;
        std::size_t transformQueueCapacity = 2048;  // oldest dropped first when full
    };

    // A remote transform with the local monotonic time it was received.
    struct RemoteSample
    {
        PlayerId player = kInvalidPlayerId;
        TimedTransform transform;
    };

    struct WorkerStatus
    {
        ClientState state = ClientState::Idle;
        PlayerId localPlayer = kInvalidPlayerId;
        std::uint32_t rttMs = 0;
        std::size_t peers = 0;
        std::uint64_t droppedEvents = 0;
    };

    // Owns the network thread. All transports and session objects live on that
    // thread; the game thread talks to it only through the thread-safe methods
    // below, which never block on network I/O.
    class NetworkWorker
    {
    public:
        explicit NetworkWorker(WorkerConfig config);
        ~NetworkWorker();

        NetworkWorker(const NetworkWorker&) = delete;
        NetworkWorker& operator=(const NetworkWorker&) = delete;

        // Starts the thread and waits for transport/session initialisation.
        bool Start(std::string& error);
        // Graceful leave, brief flush, join. Idempotent. Must not be called while
        // the OS loader lock is held (i.e. not from DllMain).
        void Stop();

        // ---- game-thread API (thread-safe, non-blocking) ----

        // Session/roster events in order. Transforms are delivered separately.
        std::size_t DrainEvents(std::vector<ClientEvent>& out, std::size_t max);
        // Every remote transform received since the last call, oldest first
        // (bounded; if the game thread stalls, the oldest samples are dropped).
        void TakeRemoteTransforms(std::vector<RemoteSample>& out);
        // Latest local transform; the network thread sends at most one per tick.
        void SubmitLocalTransform(std::uint32_t locationId, const Vec3& position, const Vec3& rotation);
        // True once if events were dropped because the game thread fell behind;
        // the caller should then rebuild its state from Roster().
        bool ConsumeOverflow() noexcept { return _overflow.exchange(false); }
        [[nodiscard]] std::map<PlayerId, std::string> Roster() const;
        [[nodiscard]] WorkerStatus Status() const;
        [[nodiscard]] bool Running() const noexcept { return _running.load(); }

    private:
        struct LocalTransform
        {
            std::uint32_t locationId = 0;
            Vec3 position;
            Vec3 rotation;
        };

        void Run(std::promise<std::string>& ready);

        WorkerConfig _config;
        std::thread _thread;
        std::atomic<bool> _stopRequested{ false };
        std::atomic<bool> _running{ false };
        std::atomic<bool> _overflow{ false };

        BoundedQueue<ClientEvent> _events;
        BoundedQueue<RemoteSample> _remoteTransforms;

        std::mutex _localMutex;
        std::optional<LocalTransform> _localTransform;

        mutable std::mutex _statusMutex;
        WorkerStatus _status;
        std::map<PlayerId, std::string> _roster;
    };
}
