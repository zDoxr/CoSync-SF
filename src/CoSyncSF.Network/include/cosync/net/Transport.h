#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cosync::net
{
    // Transport-local connection handle. 0 is never valid.
    using ConnectionId = std::uint64_t;
    inline constexpr ConnectionId kInvalidConnection = 0;

    enum class Delivery
    {
        Reliable,    // ordered, retransmitted: session/control/gameplay events
        Unreliable,  // may drop or reorder: high-frequency state (transforms)
    };

    // Application close codes. GNS reserves [1000, 1999] for application use;
    // the loopback transport uses the same values so behaviour is identical.
    enum class CloseCode : int
    {
        Normal = 1000,
        Rejected = 1001,
        ProtocolViolation = 1002,
        TimedOut = 1003,
        ServerShutdown = 1004,
        RateLimited = 1005,
        Replaced = 1006,   // superseded by a resumed connection for the same player
        Oversize = 1007,
    };

    enum class TransportEventType
    {
        Connected,     // connection fully established (both directions usable)
        Disconnected,  // closed by peer, failed, or closed locally via Close()
        Message,
    };

    struct TransportEvent
    {
        TransportEventType type = TransportEventType::Message;
        ConnectionId connection = kInvalidConnection;
        std::vector<std::uint8_t> data;  // Message only
        int closeCode = 0;               // Disconnected only
        std::string closeReason;         // Disconnected only, diagnostic text
        bool closedLocally = false;      // Disconnected only
    };

    struct Endpoint
    {
        std::string host;  // numeric IPv4/IPv6 address
        std::uint16_t port = 0;

        // Accepts "1.2.3.4:5", "[::1]:5", or a bare host with defaultPort.
        static std::optional<Endpoint> Parse(std::string_view text, std::uint16_t defaultPort = 0);
        [[nodiscard]] std::string ToString() const;
    };

    struct ConnectionStats
    {
        int pingMs = -1;
        float qualityLocal = -1.0f;  // fraction of packets delivered, 0..1, -1 = unknown
        int pendingReliableBytes = 0;
    };

    // A message transport. Not thread-safe: an instance must be created, polled,
    // used and destroyed from a single owner thread (the "network thread").
    class ITransport
    {
    public:
        virtual ~ITransport() = default;

        // Start accepting incoming connections. Returns false and sets error on failure.
        virtual bool Listen(std::uint16_t port, std::string& error) = 0;

        // Begin an outgoing connection. Completion or failure is reported as a
        // Connected/Disconnected event from Poll().
        virtual ConnectionId Connect(const Endpoint& endpoint, std::string& error) = 0;

        // Queue a message. Returns false if the connection is unknown or the send failed.
        virtual bool Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery) = 0;

        // Close a connection. With linger, queued reliable data is flushed first.
        // A Disconnected event with closedLocally = true is reported by the next Poll().
        virtual void Close(ConnectionId connection, CloseCode code, std::string_view reason, bool linger) = 0;

        // Collect pending events. Messages larger than maxMessageBytes are dropped
        // and their connection is closed with CloseCode::Oversize.
        virtual void Poll(std::vector<TransportEvent>& out) = 0;

        // Close everything and stop listening. Safe to call more than once.
        virtual void Shutdown() = 0;

        virtual std::optional<ConnectionStats> GetStats(ConnectionId connection) = 0;
        [[nodiscard]] virtual std::string Describe(ConnectionId connection) = 0;
    };
}
