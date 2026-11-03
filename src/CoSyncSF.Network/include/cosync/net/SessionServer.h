#pragma once

#include "cosync/net/GroupManager.h"
#include "cosync/net/Transport.h"
#include "cosync/net/Util.h"
#include "cosync/shared/Protocol.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cosync::net
{
    struct ServerConfig
    {
        std::string serverName = "CoSync-SF Session";
        std::uint16_t maxPlayers = 8;
        std::string password;                 // empty = no password
        std::uint32_t requiredGameRuntime = 0;  // 0 = accept any (e.g. tool clients); else exact match
        std::uint32_t handshakeTimeoutMs = 5000;
        std::uint16_t heartbeatIntervalMs = 1000;
        std::uint32_t idleTimeoutMs = 10000;   // no message from a player for this long => dropped
        std::uint32_t resumeWindowMs = 30000;  // how long a dropped player's slot is reserved
        std::uint32_t maxPendingConnections = 16;
        double maxMessagesPerSecond = 120.0;
        double messageBurst = 240.0;
    };

    struct ServerStats
    {
        std::uint64_t messagesReceived = 0;
        std::uint64_t decodeFailures = 0;
        std::uint64_t protocolViolations = 0;
        std::uint64_t transformsRelayed = 0;
        std::uint64_t staleTransformsDropped = 0;
        std::uint64_t rateLimitDisconnects = 0;
        std::uint64_t rejections = 0;
    };

    struct PlayerInfo
    {
        PlayerId id = kInvalidPlayerId;
        std::string name;
        bool connected = false;  // false while the slot is reserved for resume
        std::uint32_t gameRuntime = 0;
    };

    // Session authority: handshake/version negotiation, player registration,
    // heartbeats, resume after drops, transform relay and abuse limits. It does
    // NOT simulate Starfield; game state is owned by the game clients.
    //
    // Single-threaded: construct, Update() and destroy on the transport's thread.
    class SessionServer
    {
    public:
        SessionServer(ITransport& transport, ServerConfig config, Clock clock = SteadyNowMs);
        ~SessionServer();

        SessionServer(const SessionServer&) = delete;
        SessionServer& operator=(const SessionServer&) = delete;

        bool Start(std::uint16_t port, std::string& error);
        void Update();
        // Notifies every connection and closes it with linger. The transport
        // still needs polling briefly afterwards to flush.
        void Stop();

        bool Kick(PlayerId player, std::string_view reason);

        [[nodiscard]] std::vector<PlayerInfo> Players() const;
        [[nodiscard]] std::size_t ConnectedPlayerCount() const;
        [[nodiscard]] std::size_t PendingConnectionCount() const;
        [[nodiscard]] SessionId Id() const noexcept { return _sessionId; }
        [[nodiscard]] const ServerStats& Stats() const noexcept { return _stats; }
        [[nodiscard]] const ServerConfig& Config() const noexcept { return _config; }

    private:
        struct Connection
        {
            ConnectionId id = kInvalidConnection;
            std::uint64_t openedAt = 0;
            std::uint64_t lastReceived = 0;
            PlayerId player = kInvalidPlayerId;  // 0 until Hello accepted
            TokenBucket bucket;
        };

        struct Player
        {
            PlayerId id = kInvalidPlayerId;
            std::string name;
            ResumeToken token;
            ConnectionId connection = kInvalidConnection;  // invalid while reserved
            std::uint64_t reservedUntil = 0;
            std::uint32_t gameRuntime = 0;
            std::uint32_t lastTransformSequence = 0;
            bool hasTransform = false;
        };

        void OnConnected(ConnectionId id);
        void OnDisconnected(const TransportEvent& ev);
        void OnMessage(ConnectionId id, std::span<const std::uint8_t> data);
        void OnHello(Connection& conn, const proto::Hello& hello);
        void OnTransform(Connection& conn, Player& player, proto::PlayerTransform transform);
        void CheckTimers();

        void Send(ConnectionId id, const proto::Message& msg, Delivery delivery = Delivery::Reliable);
        void Broadcast(const proto::Message& msg, PlayerId except, Delivery delivery = Delivery::Reliable);
        void RejectAndClose(ConnectionId id, proto::RejectReason reason, std::string_view detail);

        // Closes a connection. If it belongs to a player, either removes the player
        // (leaveReason set) or reserves the slot for resume (leaveReason empty).
        void DropConnection(ConnectionId id, CloseCode code, std::string_view detail,
                            std::optional<proto::LeaveReason> leaveReason);
        void RemovePlayer(PlayerId id, proto::LeaveReason reason);
        void ReservePlayer(Player& player);

        void Welcome(Connection& conn, Player& player, bool resumed);
        [[nodiscard]] Player* FindByToken(const ResumeToken& token);
        [[nodiscard]] bool NameTaken(std::string_view name) const;
        [[nodiscard]] ResumeToken NewToken();

        ITransport& _transport;
        ServerConfig _config;
        Clock _clock;
        SessionId _sessionId = 0;
        PlayerId _nextPlayerId = 1;
        bool _running = false;
        ServerStats _stats;
        std::unordered_map<ConnectionId, Connection> _connections;
        std::unordered_map<PlayerId, Player> _players;
        GroupManager _groups;
        std::vector<TransportEvent> _events;
    };
}
