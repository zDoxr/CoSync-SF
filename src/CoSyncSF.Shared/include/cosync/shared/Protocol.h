#pragma once

#include "cosync/shared/Types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// CoSync-SF wire protocol.
//
// Every message is: u16 type | u16 flags (must be 0) | type-specific payload.
// All integers are little-endian; strings are u8-length-prefixed UTF-8 with
// per-field limits; a decoded message must consume the buffer exactly.
// Transport framing (message boundaries, reliability, encryption) is provided
// by the transport layer, so there is no length field here.
namespace cosync::proto
{
    inline constexpr std::uint32_t kMagic = 0x46535343;  // "CSSF" as little-endian bytes
    inline constexpr std::uint16_t kProtocolMajor = 1;   // incompatible changes
    inline constexpr std::uint16_t kProtocolMinor = 0;   // backward-compatible additions

    inline constexpr std::size_t kMaxMessageBytes = 16 * 1024;
    inline constexpr std::size_t kMaxPlayerNameBytes = 32;
    inline constexpr std::size_t kMaxServerNameBytes = 64;
    inline constexpr std::size_t kMaxBuildBytes = 64;
    inline constexpr std::size_t kMaxPasswordBytes = 64;
    inline constexpr std::size_t kMaxDetailBytes = 128;
    inline constexpr std::uint16_t kMaxPlayersLimit = 64;

    // World coordinates beyond this are rejected as nonsense. Starfield cell
    // coordinates are far smaller; this is an abuse bound, not a gameplay rule.
    inline constexpr float kMaxCoordinate = 1.0e7f;

    enum class MessageType : std::uint16_t
    {
        Hello = 1,            // C->S reliable: first message on every connection
        Welcome = 2,          // S->C reliable: registration accepted
        Reject = 3,           // S->C reliable: registration refused, connection closes
        SessionSnapshot = 4,  // S->C reliable: current roster after Welcome
        PlayerJoined = 5,     // S->C reliable
        PlayerLeft = 6,       // S->C reliable
        Heartbeat = 7,        // C->S reliable
        HeartbeatAck = 8,     // S->C reliable
        Disconnect = 9,       // both directions, reliable: graceful leave / shutdown
        PlayerTransform = 10, // C->S then S->C relay, unreliable
    };

    enum class RejectReason : std::uint16_t
    {
        ProtocolVersionMismatch = 1,
        GameVersionMismatch = 2,
        ServerFull = 3,
        InvalidName = 4,
        NameInUse = 5,
        BadPassword = 6,
        ProtocolViolation = 7,
        HandshakeTimeout = 8,
        ServerShuttingDown = 9,
        RateLimited = 10,
    };

    enum class LeaveReason : std::uint8_t
    {
        Quit = 1,              // graceful Disconnect from the player
        ConnectionLost = 2,    // dropped; may return with the same PlayerId via resume
        TimedOut = 3,
        Kicked = 4,
        ProtocolViolation = 5,
        ServerShutdown = 6,
    };

    enum class DisconnectReason : std::uint16_t
    {
        Quit = 1,
        ServerShutdown = 2,
        Kicked = 3,
    };

    struct Hello
    {
        std::uint32_t magic = kMagic;
        std::uint16_t protocolMajor = kProtocolMajor;
        std::uint16_t protocolMinor = kProtocolMinor;
        std::uint32_t gameRuntime = 0;   // packed runtime version, 0 = not running in game
        std::string clientBuild;
        std::string playerName;
        std::string password;            // optional shared session password (LAN mode)
        ResumeToken resumeToken;         // zero for a fresh join
    };

    struct Welcome
    {
        std::uint16_t protocolMajor = kProtocolMajor;
        std::uint16_t protocolMinor = kProtocolMinor;
        PlayerId playerId = kInvalidPlayerId;
        SessionId sessionId = 0;
        ResumeToken resumeToken;
        std::string serverName;
        std::uint16_t maxPlayers = 0;
        std::uint16_t heartbeatIntervalMs = 0;
        bool resumed = false;
    };

    struct Reject
    {
        RejectReason reason = RejectReason::ProtocolViolation;
        std::uint16_t serverProtocolMajor = kProtocolMajor;
        std::uint16_t serverProtocolMinor = kProtocolMinor;
        std::string detail;
    };

    struct PlayerEntry
    {
        PlayerId playerId = kInvalidPlayerId;
        std::string name;
    };

    struct SessionSnapshot
    {
        std::vector<PlayerEntry> players;
    };

    struct PlayerJoined
    {
        PlayerEntry player;
    };

    struct PlayerLeft
    {
        PlayerId playerId = kInvalidPlayerId;
        LeaveReason reason = LeaveReason::Quit;
    };

    struct Heartbeat
    {
        std::uint32_t sequence = 0;
        std::uint64_t sendTimeMs = 0;
    };

    struct HeartbeatAck
    {
        std::uint32_t sequence = 0;
        std::uint64_t echoTimeMs = 0;
    };

    struct Disconnect
    {
        DisconnectReason reason = DisconnectReason::Quit;
        std::string detail;
    };

    // Position/rotation of a player character. Clients send playerId = 0 (the
    // server stamps the authenticated id before relaying). locationId is an
    // opaque location key; its mapping to Starfield cells/worldspaces is defined
    // by the game-integration layer and is not interpreted by the server.
    struct PlayerTransform
    {
        PlayerId playerId = kInvalidPlayerId;
        std::uint32_t sequence = 0;
        std::uint32_t locationId = 0;
        Vec3 position;
        Vec3 rotation;  // radians
    };

    using Message = std::variant<Hello, Welcome, Reject, SessionSnapshot, PlayerJoined, PlayerLeft,
                                 Heartbeat, HeartbeatAck, Disconnect, PlayerTransform>;

    enum class DecodeError
    {
        None,
        Empty,
        TooLarge,
        Truncated,
        TrailingBytes,
        UnknownType,
        BadFlags,
        InvalidField,
    };

    struct DecodeResult
    {
        std::optional<Message> message;
        DecodeError error = DecodeError::None;

        [[nodiscard]] bool Ok() const noexcept { return message.has_value(); }
    };

    [[nodiscard]] std::vector<std::uint8_t> Encode(const Message& message);
    [[nodiscard]] DecodeResult Decode(std::span<const std::uint8_t> bytes);

    [[nodiscard]] MessageType TypeOf(const Message& message) noexcept;
    [[nodiscard]] std::string_view ToString(MessageType type) noexcept;
    [[nodiscard]] std::string_view ToString(DecodeError error) noexcept;
    [[nodiscard]] std::string_view ToString(RejectReason reason) noexcept;
    [[nodiscard]] std::string_view ToString(LeaveReason reason) noexcept;

    // Field validation shared by encoder users and the decoder.
    [[nodiscard]] bool IsValidText(std::string_view s, std::size_t maxBytes) noexcept;
    [[nodiscard]] bool IsValidPlayerName(std::string_view name) noexcept;
    [[nodiscard]] bool IsValidTransform(const PlayerTransform& t) noexcept;

    // Serial-number comparison (RFC 1982 style) for wrapping u32 sequences.
    [[nodiscard]] constexpr bool SequenceNewer(std::uint32_t candidate, std::uint32_t last) noexcept
    {
        return candidate != last && static_cast<std::int32_t>(candidate - last) > 0;
    }
}
