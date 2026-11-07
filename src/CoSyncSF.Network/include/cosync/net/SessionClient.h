#pragma once

#include "cosync/net/Transport.h"
#include "cosync/net/Util.h"
#include "cosync/shared/Protocol.h"

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace cosync::net
{
    struct ClientConfig
    {
        std::string playerName;
        std::string password;
        std::uint32_t gameRuntime = 0;   // packed Starfield runtime, 0 for tool clients
        std::string clientBuild = "CoSyncSF/0.1.0";
        std::uint32_t connectTimeoutMs = 10000;
        std::uint32_t handshakeTimeoutMs = 5000;
        std::uint32_t serverTimeoutMs = 10000;  // silence from server => connection considered lost
        bool autoReconnect = true;             // resume after an unexpected drop
        std::uint32_t reconnectDelayMs = 1000;
        std::uint32_t maxReconnectAttempts = 5;
    };

    enum class ClientState
    {
        Idle,
        Connecting,    // transport connecting
        Handshaking,   // Hello sent, awaiting Welcome/Reject
        InSession,
        Reconnecting,  // waiting to retry with the resume token
        Disconnected,  // terminal until Connect() is called again
    };

    [[nodiscard]] std::string_view ToString(ClientState state) noexcept;

    namespace client_event
    {
        struct StateChanged { ClientState state; std::string detail; };
        struct Welcomed { proto::Welcome welcome; };
        struct Rejected { proto::Reject reject; };
        struct PeerJoined { proto::PlayerEntry player; };
        struct PeerLeft { PlayerId playerId; proto::LeaveReason reason; };
        struct RemoteTransform { proto::PlayerTransform transform; };
        struct RttSample { std::uint32_t rttMs; };
        struct NpcCombat { proto::NpcCombat combat; };
        struct Groups { proto::GroupState state; };
        struct NpcAuthority { PlayerId authority; };
        struct NpcStates { proto::NpcStates states; };
        struct NpcDeath { proto::NpcDeath death; };
        struct NpcDamage { proto::NpcDamage damage; };
        struct NpcEvents { proto::NpcEvents events; };
        struct NpcOutfits { proto::NpcOutfits outfits; };
        struct WorldEvents { proto::WorldEvents world; };
        struct WorldClock { proto::WorldClock clock; };
    }

    using ClientEvent = std::variant<client_event::StateChanged, client_event::Welcomed, client_event::Rejected,
                                     client_event::PeerJoined, client_event::PeerLeft, client_event::RemoteTransform,
                                     client_event::RttSample, client_event::NpcCombat, client_event::Groups,
                                     client_event::NpcAuthority, client_event::NpcStates, client_event::NpcDeath,
                                     client_event::NpcDamage, client_event::NpcEvents, client_event::NpcOutfits, client_event::WorldEvents, client_event::WorldClock>;

    // Client side of the session protocol. Single-threaded; see ITransport.
    class SessionClient
    {
    public:
        SessionClient(ITransport& transport, ClientConfig config, Clock clock = SteadyNowMs);
        ~SessionClient();

        SessionClient(const SessionClient&) = delete;
        SessionClient& operator=(const SessionClient&) = delete;

        bool Connect(const Endpoint& endpoint, std::string& error);
        void Update(std::vector<ClientEvent>& events);
        // Graceful leave: tells the server, closes, and forgets the resume token.
        void Disconnect();

        // Sends the local player's transform (unreliable). Only valid InSession.
        // Reports an NPC that entered combat against `targetPlayer` here (reliable).
        bool SendNpcCombat(std::uint32_t npcFormId, PlayerId targetPlayer);
        bool SendGroupRequest(const proto::GroupRequest& request);
        // NPC replication (1.6). States and deaths are refused by the server unless this client is
        // the NPC authority.
        bool SendNpcStates(std::uint32_t locationId, std::vector<proto::NpcState> npcs);
        bool SendNpcDeath(std::uint32_t npcFormId);
        bool SendNpcDamage(std::uint32_t npcFormId, float amount);
        bool SendNpcEvents(std::vector<proto::NpcEvent> events);
        bool SendNpcOutfits(std::vector<proto::NpcOutfit> outfits);
        bool SendWorldEvents(std::vector<proto::WorldEvent> events);
        bool SendWorldClock(const proto::WorldClock& clock);
        [[nodiscard]] PlayerId NpcAuthority() const noexcept { return _npcAuthority; }
        bool SendTransform(std::uint32_t locationId, const Vec3& position, const Vec3& rotation,
                           const std::optional<proto::PlayerControls>& controls = std::nullopt,
                           const std::vector<proto::AnimEvent>& events = {},
                           const std::optional<proto::WeaponState>& weapon = std::nullopt,
                           const std::optional<proto::PlayerStatus>& status = std::nullopt);

        [[nodiscard]] ClientState State() const noexcept { return _state; }
        [[nodiscard]] PlayerId LocalPlayerId() const noexcept { return _playerId; }
        [[nodiscard]] SessionId CurrentSessionId() const noexcept { return _sessionId; }
        [[nodiscard]] const std::map<PlayerId, std::string>& Roster() const noexcept { return _roster; }
        [[nodiscard]] std::uint32_t LastRttMs() const noexcept { return _lastRttMs; }

    private:
        void SetState(ClientState state, std::string detail, std::vector<ClientEvent>& events);
        void OnMessage(std::span<const std::uint8_t> data, std::vector<ClientEvent>& events);
        void ApplySnapshot(const proto::SessionSnapshot& snapshot, std::vector<ClientEvent>& events);
        void ConnectionLost(std::string detail, std::vector<ClientEvent>& events);
        void Fail(std::string detail, std::vector<ClientEvent>& events);
        void Send(const proto::Message& msg, Delivery delivery = Delivery::Reliable);
        void CloseConnection(CloseCode code, std::string_view reason, bool linger);
        bool StartConnection(std::string& error);

        ITransport& _transport;
        ClientConfig _config;
        Clock _clock;

        ClientState _state = ClientState::Idle;
        Endpoint _endpoint;
        ConnectionId _connection = kInvalidConnection;
        std::uint64_t _stateSince = 0;
        std::uint64_t _lastReceived = 0;
        std::uint64_t _nextHeartbeat = 0;
        std::uint64_t _nextReconnect = 0;
        std::uint32_t _reconnectAttempts = 0;
        bool _reportConnecting = false;

        PlayerId _playerId = kInvalidPlayerId;
        SessionId _sessionId = 0;
        ResumeToken _resumeToken;
        std::uint16_t _heartbeatIntervalMs = 1000;
        std::uint32_t _heartbeatSequence = 0;
        std::uint32_t _transformSequence = 0;
        std::uint32_t _lastRttMs = 0;

        std::map<PlayerId, std::string> _roster;  // remote players only
        std::map<PlayerId, std::uint32_t> _lastRemoteSequence;
        PlayerId _npcAuthority = kInvalidPlayerId;
        std::uint32_t _npcStatesSequence = 0;  // outgoing
        std::uint32_t _lastNpcSequence = 0;    // incoming, from the current authority
        bool _haveNpcSequence = false;
        std::vector<TransportEvent> _transportEvents;
    };
}
