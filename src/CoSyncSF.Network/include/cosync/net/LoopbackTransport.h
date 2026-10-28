#pragma once

#include "cosync/net/Transport.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_map>

namespace cosync::net
{
    class LoopbackTransport;

    // Shared in-process "network" for LoopbackTransport instances. Used by tests
    // to exercise session logic deterministically without sockets. All
    // transports attached to one hub must be used from the same thread.
    class LoopbackHub
    {
    public:
        LoopbackHub() = default;
        LoopbackHub(const LoopbackHub&) = delete;
        LoopbackHub& operator=(const LoopbackHub&) = delete;

    private:
        friend class LoopbackTransport;

        struct Link
        {
            LoopbackTransport* owner = nullptr;
            ConnectionId peer = kInvalidConnection;
        };

        std::unordered_map<std::uint16_t, LoopbackTransport*> _listeners;
        std::unordered_map<ConnectionId, Link> _links;
        ConnectionId _nextId = 1;
    };

    class LoopbackTransport final : public ITransport
    {
    public:
        explicit LoopbackTransport(std::shared_ptr<LoopbackHub> hub, std::size_t maxMessageBytes);
        ~LoopbackTransport() override;

        LoopbackTransport(const LoopbackTransport&) = delete;
        LoopbackTransport& operator=(const LoopbackTransport&) = delete;

        bool Listen(std::uint16_t port, std::string& error) override;
        ConnectionId Connect(const Endpoint& endpoint, std::string& error) override;
        bool Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery) override;
        void Close(ConnectionId connection, CloseCode code, std::string_view reason, bool linger) override;
        void Poll(std::vector<TransportEvent>& out) override;
        void Shutdown() override;
        std::optional<ConnectionStats> GetStats(ConnectionId connection) override;
        [[nodiscard]] std::string Describe(ConnectionId connection) override;

        // Test hook: silently discard unreliable sends (simulates packet loss).
        void SetDropUnreliable(bool drop) noexcept { _dropUnreliable = drop; }
        // Test hook: sever every connection of this transport as a network failure
        // would; both ends see Disconnected with closedLocally = false.
        void SimulateNetworkFailure();

    private:
        void Push(TransportEvent ev) { _pending.push_back(std::move(ev)); }

        std::shared_ptr<LoopbackHub> _hub;
        std::size_t _maxMessageBytes;
        std::uint16_t _listenPort = 0;
        std::deque<TransportEvent> _pending;
        bool _dropUnreliable = false;
    };
}
