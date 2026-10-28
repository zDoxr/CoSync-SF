#pragma once

#include "cosync/net/Transport.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace cosync::net
{
    struct GnsOptions
    {
        std::size_t maxMessageBytes = 16 * 1024;
        int recvBufferBytes = 256 * 1024;  // per connection; bounds memory a peer can pin
        int recvBufferMessages = 1024;     // per connection
        int connectTimeoutMs = 10000;
        int idleTimeoutMs = 10000;         // transport-level; sessions also run their own heartbeat
    };

    // ITransport over Valve's standalone GameNetworkingSockets (direct IP / LAN).
    // Connections are encrypted by GNS, but peers are NOT authenticated (no
    // certificate authority is configured); identity must come from the session
    // layer. Steam identity/relay is a separate backend (see docs/RESEARCH.md).
    //
    // GNS dispatches status callbacks from the process-wide RunCallbacks(), on
    // whichever thread calls it. The callback therefore only queues a copy for
    // the owning transport, which applies it on its own thread in Poll(). Each
    // instance is still single-owner-thread, but several instances may run on
    // different threads.
    class GnsTransport final : public ITransport
    {
    public:
        explicit GnsTransport(GnsOptions options = {});
        ~GnsTransport() override;

        GnsTransport(const GnsTransport&) = delete;
        GnsTransport& operator=(const GnsTransport&) = delete;

        // False if the GNS library failed to initialise; see InitError().
        [[nodiscard]] bool IsReady() const noexcept { return _ready; }
        [[nodiscard]] const std::string& InitError() const noexcept { return _initError; }

        bool Listen(std::uint16_t port, std::string& error) override;
        ConnectionId Connect(const Endpoint& endpoint, std::string& error) override;
        bool Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery) override;
        void Close(ConnectionId connection, CloseCode code, std::string_view reason, bool linger) override;
        void Poll(std::vector<TransportEvent>& out) override;
        void Shutdown() override;
        std::optional<ConnectionStats> GetStats(ConnectionId connection) override;
        [[nodiscard]] std::string Describe(ConnectionId connection) override;

        // Plain-data copy of a GNS status callback (keeps GNS types out of this header).
        struct StatusChange
        {
            std::uint32_t connection = 0;
            int state = 0;
            std::uint32_t listenSocket = 0;
            int endReason = 0;
            std::string endDebug;
        };
        // Internal: called by the GNS callback trampoline on any thread.
        void QueueStatusChange(StatusChange change);

    private:
        struct ConnState
        {
            bool outgoing = false;
            bool announced = false;  // Connected event delivered
        };

        void ApplyStatusChange(const StatusChange& change);
        void DrainConnection(std::uint32_t connection);

        GnsOptions _options;
        bool _ready = false;
        std::string _initError;
        std::uint32_t _listenSocket = 0;  // HSteamListenSocket
        std::uint32_t _pollGroup = 0;     // HSteamNetPollGroup

        // Owner-thread state.
        std::unordered_map<std::uint32_t, ConnState> _connections;
        std::vector<TransportEvent> _pending;

        // Filled from the callback trampoline (any thread).
        std::mutex _statusMutex;
        std::vector<StatusChange> _statusQueue;
    };
}
