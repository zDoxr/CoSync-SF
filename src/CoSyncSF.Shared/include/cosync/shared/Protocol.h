#pragma once

#include "cosync/shared/Types.h"

#include <array>
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
    inline constexpr std::uint16_t kProtocolMinor = 16;   // backward-compatible additions
                                                         // 1.1: optional PlayerTransform::controls
                                                         // 1.2: tagged trailing sections, PlayerTransform::events
                                                         // 1.3: PlayerTransform::weapon
                                                         // 1.4: NpcCombat
                                                         // 1.5: PlayerTransform::status, groups
                                                         // 1.6: NPC authority, states, death, damage
                                                         // 1.7: NPC graph events, equipment

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
        NpcCombat = 11,       // C->S then S->C relay, reliable (1.4)
        GroupRequest = 12,    // C->S reliable (1.5)
        GroupState = 13,      // S->C reliable, authoritative group list (1.5)
        NpcAuthority = 14,    // S->C reliable, who owns NPC simulation (1.6)
        NpcStates = 15,       // authority->S->others, unreliable (1.6)
        NpcDeath = 16,        // authority->S->others, reliable; replayed to late joiners (1.6)
        NpcDamage = 17,       // C->S->authority, reliable (1.6)
        NpcEvents = 18,       // authority->S->others, reliable (1.7)
        NpcOutfits = 19,        // authority->S->others, reliable (1.7)
        WorldEvents = 20,       // C->S->others, reliable; replayed to late joiners (1.10)
        WorldClock = 21,        // authority->S->others, reliable (1.10)
        DialogueLine = 22,      // C->S->players nearby, reliable (1.14)
        DroppedItem = 23,       // C->S->others, reliable (1.16)
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
    // The sender's movement input as the game engine computed it for its own character
    // (Starfield ControlsData, the result of IMovementPlayerControls; see
    // docs/ANIMATION_REVERSE_ENGINEERING.md A.10). Opaque to the server; receivers replay it into
    // the remote player's proxy so the engine animates it like a player.
    // Bytes +0x00..+0x53 as 21 floats, +0x54 as a flag byte.
    inline constexpr std::size_t kControlFloats = 21;
    inline constexpr float kMaxControlValue = 1.0e4f;  // abuse bound; real values are angles/scales
    struct PlayerControls
    {
        std::array<float, kControlFloats> values{};
        std::uint8_t flag = 0;  // 0 or 1
    };

    // An animation-graph input event the sender's engine sent to its own character (jump, sneak,
    // attack, ...). `sequence` increases by one per event (wrapping); senders repeat recent events
    // in several consecutive transforms and receivers drop sequences they have already seen.
    inline constexpr std::size_t kMaxAnimEventsPerTransform = 8;
    inline constexpr std::size_t kMaxAnimEventNameBytes = 48;
    struct AnimEvent
    {
        std::uint16_t sequence = 0;
        std::string name;
    };

    // The sender's equipped weapon (game FormID, 0 = none) and whether it is drawn.
    struct WeaponState
    {
        std::uint32_t formId = 0;
        bool drawn = false;
    };

    // The sender's vitals and whereabouts, for party frames and overhead markers (1.5).
    inline constexpr std::size_t kMaxPlaceBytes = 64;
    enum class PlaceKind : std::uint8_t { Unknown = 0, Surface = 1, Space = 2 };
    struct PlayerStatus
    {
        std::uint8_t healthPercent = 100;  // 0..100
        std::uint16_t level = 1;
        PlaceKind kind = PlaceKind::Unknown;
        std::string place;                 // planet name (or empty)
    };

    // The sender's ship while they are in space (1.12): enough for others to show a stand-in of the
    // same design where it is. Space cells use larger coordinates than the ground.
    inline constexpr float kMaxShipCoordinate = 1.0e9f;
    struct ShipState
    {
        std::uint32_t cellId = 0;  // stable location key of the ship's cell, as locationId
        std::uint32_t baseId = 0;  // the ship's base form: its design
        Vec3 position;
        Vec3 rotation;  // radians
    };

    struct PlayerTransform
    {
        PlayerId playerId = kInvalidPlayerId;
        std::uint32_t sequence = 0;
        std::uint32_t locationId = 0;
        Vec3 position;
        Vec3 rotation;  // radians
        // Optional trailing sections, each at most once, any order: u8 tag + body.
        //   tag 1 (1.1+): controls = 21 x f32, u8 flag
        //   tag 2 (1.2+): events   = u8 count (1..8), count x { u16 sequence, string name }
        //   tag 3 (1.3+): weapon   = u32 formId, u8 drawn (0/1)
        //   tag 4 (1.5+): status   = u8 health%, u16 level, u8 kind, string place
        //   tag 5 (1.12+): ship    = u32 cellId, u32 baseId, 3 x f32 position, 3 x f32 rotation
        std::optional<PlayerControls> controls;
        std::vector<AnimEvent> events;
        std::optional<WeaponState> weapon;
        std::optional<PlayerStatus> status;
        std::optional<ShipState> ship;
    };

    // An NPC (data-file reference, identical FormID on every machine with the same load order)
    // entered combat against a player on the reporting machine. Clients send playerId = 0; the
    // server stamps the reporter before relaying. targetPlayer is the player the NPC fights.
    struct NpcCombat
    {
        PlayerId playerId = kInvalidPlayerId;
        std::uint32_t npcFormId = 0;
        PlayerId targetPlayer = kInvalidPlayerId;
    };

    // NPC replication (1.6, docs/NPC_SYNC_ARCHITECTURE.md). One game client at a time is the NPC
    // authority, chosen by the server. Data-file references carry the same FormID on every machine
    // with the same load order. Runtime FF references only match between copies of one save, so
    // receivers must also compare baseId before treating one as the same NPC.
    inline constexpr std::size_t kMaxNpcStates = 100;  // per message; a snapshot spans several
    inline constexpr float kMaxNpcDamage = 1.0e6f;    // abuse bound, in health points
    [[nodiscard]] constexpr bool IsRuntimeFormId(std::uint32_t formId) noexcept { return (formId >> 24) == 0xFF; }
    [[nodiscard]] constexpr bool IsReplicatedNpcId(std::uint32_t formId) noexcept { return formId != 0; }

    struct NpcAuthority
    {
        PlayerId authority = kInvalidPlayerId;  // none = no game client connected
    };

    enum NpcFlags : std::uint8_t
    {
        kNpcDead = 1 << 0,
        kNpcWeaponDrawn = 1 << 1,
        kNpcInCombat = 1 << 2,
        kNpcSneaking = 1 << 3,
        kNpcInScene = 1 << 4,
        kNpcUnconscious = 1 << 5,
        kNpcSitting = 1 << 6,  // in furniture
        kNpcCrowdTarget = 1 << 7,  // ambient crowd actor heading for crowdTarget
    };
    inline constexpr std::uint8_t kNpcKnownFlags = 0xFF;

    struct NpcState
    {
        std::uint32_t formId = 0;
        std::uint32_t baseId = 0;
        Vec3 position;
        float yaw = 0.0f;  // radians
        std::uint8_t healthPercent = 100;  // 0..100
        std::uint8_t flags = 0;
        // Where CrowdManager is sending it (1.9). On the wire only with kNpcCrowdTarget. A position,
        // because the reference itself may be a runtime one, numbered per copy (RE 12.10).
        Vec3 crowdTarget;
    };

    // Part of the latest state of the NPCs the authority has loaded, nearest first. Clients send
    // playerId = 0; the server stamps the sender and refuses anyone but the authority.
    struct NpcStates
    {
        PlayerId playerId = kInvalidPlayerId;
        std::uint32_t sequence = 0;
        std::uint32_t locationId = 0;
        std::vector<NpcState> npcs;
    };

    struct NpcDeath
    {
        PlayerId playerId = kInvalidPlayerId;
        std::uint32_t npcFormId = 0;
    };

    // Health a client's player took from its local copy of an NPC. The authority's engine applies
    // it, so damage, bleedout and death are decided once. Clients send playerId = 0.
    struct NpcDamage
    {
        PlayerId playerId = kInvalidPlayerId;
        std::uint32_t npcFormId = 0;
        float amount = 0.0f;  // health points, > 0
    };

    // Animation-graph input events the authority's engine sent to its NPCs, in order.
    inline constexpr std::size_t kMaxNpcEvents = 64;
    struct NpcEvent
    {
        std::uint32_t formId = 0;
        std::string name;
    };
    struct NpcEvents
    {
        PlayerId playerId = kInvalidPlayerId;
        std::vector<NpcEvent> events;
    };

    // What NPCs wear and hold on the authority: every equipped armor and weapon, sorted. Sent as
    // full state on a timer, so a receiver corrects any difference, however it came about.
    inline constexpr std::size_t kMaxNpcOutfits = 64;      // per message
    inline constexpr std::size_t kMaxNpcOutfitItems = 24;  // per NPC
    struct NpcOutfit
    {
        std::uint32_t formId = 0;
        std::vector<std::uint32_t> items;
        // Hash of what makes the NPC look the way it does. Runtime base IDs differ between copies,
        // so this is how they tell whether two NPCs are the same person.
        std::uint32_t look = 0;
        // The data-file NPC a generated one was made from (1.13), 0 if none. A copy that has no such
        // NPC of its own places one from this template instead (issue #2).
        std::uint32_t templateId = 0;
    };
    struct NpcOutfits
    {
        PlayerId playerId = kInvalidPlayerId;
        std::vector<NpcOutfit> outfits;
    };

    // Shared world state (1.10, docs/WORLD_SYNC.md). A change one player caused. Any player may report
    // one, the server remembers it for late joiners and passes it on.
    inline constexpr std::size_t kMaxWorldEvents = 64;  // per message
    enum class WorldEventKind : std::uint8_t
    {
        DoorOpen = 1,       // formId door, itemId its base, value 1 open / 0 closed
        RefRemoved = 2,     // formId reference taken from the world, itemId its base, value how many the player got (1.15, 0 if unknown)
        QuestStage = 3,     // formId quest, value the stage set
        ContainerItem = 4,  // formId container, itemId the item, value the change in count
        LockState = 5,      // formId reference, itemId its base, value 1 locked / 0 unlocked
        Destroyed = 6,      // formId reference, itemId its base, value 1 destroyed / 0 repaired
        // S->one player only (1.15): another player took that reference first, so give back what was
        // picked up. formId the reference, itemId the item, value the count. Never remembered or relayed.
        ItemReturned = 7,
        // A dropped item (1.16) was picked up: formId its shared ID (see DroppedItem), itemId its
        // base, value how many the player got.
        SharedRemoved = 8,
    };
    struct WorldEvent
    {
        WorldEventKind kind = WorldEventKind::DoorOpen;
        std::uint32_t formId = 0;
        std::uint32_t itemId = 0;
        std::int32_t value = 0;
        bool operator==(const WorldEvent&) const = default;
    };
    struct WorldEvents
    {
        PlayerId playerId = kInvalidPlayerId;
        std::vector<WorldEvent> events;
    };
    [[nodiscard]] bool IsValidWorldEvent(const WorldEvent& e) noexcept;

    // The authority's sky, on a timer. Weather only applies to a copy in the same location.
    struct WorldClock
    {
        PlayerId playerId = kInvalidPlayerId;
        float gameDaysPassed = 0.0f;
        float gameHour = 0.0f;
        std::uint32_t locationId = 0;
        std::uint32_t weather = 0;  // 0 = unknown
    };

    // Groups (1.5): who shares party frames and overhead markers. The server owns the state.
    inline constexpr std::size_t kMaxGroups = 32;
    inline constexpr std::size_t kMaxGroupMembers = 8;
    enum class GroupAction : std::uint8_t { Create = 1, Invite = 2, Accept = 3, Decline = 4, Leave = 5, Kick = 6 };
    struct GroupRequest
    {
        GroupAction action = GroupAction::Create;
        PlayerId target = kInvalidPlayerId;  // Invite / Kick
        std::uint32_t groupId = 0;           // Accept / Decline
    };
    struct GroupInfo
    {
        std::uint32_t id = 0;
        PlayerId leader = kInvalidPlayerId;
        std::vector<PlayerId> members;  // includes the leader
        std::vector<PlayerId> invited;
        bool operator==(const GroupInfo&) const = default;
    };
    struct GroupState
    {
        std::uint32_t version = 0;
        std::vector<GroupInfo> groups;
    };

    // A line said in a conversation one player is in, as the game subtitled it (1.14). Others nearby
    // see it, so a conversation is not silent for the players watching (issue #16). Clients send
    // playerId = 0; the server stamps the sender and passes it only to players who may share the place.
    inline constexpr std::size_t kMaxSpeakerBytes = 64;
    inline constexpr std::size_t kMaxLineBytes = 512;
    struct DialogueLine
    {
        PlayerId playerId = kInvalidPlayerId;
        std::string speaker;
        std::string text;
        bool isPlayer = false;  // the sending player's own line, not the NPC's
    };

    // An item a player dropped (1.16, issue #6). The dropped reference is numbered per copy, so the
    // dropping player names it with a shared ID: their player ID in the top byte, a counter below.
    // Every other copy places the same item there, and SharedRemoved takes it away again.
    struct DroppedItem
    {
        PlayerId playerId = kInvalidPlayerId;
        std::uint32_t sharedId = 0;
        std::uint32_t baseId = 0;
        std::int32_t count = 0;
        std::uint32_t locationId = 0;  // as in PlayerTransform: the drop is placed only there
        Vec3 position{};
    };
    [[nodiscard]] constexpr std::uint32_t MakeSharedId(PlayerId player, std::uint32_t sequence) noexcept
    {
        return (static_cast<std::uint32_t>(player) << 24) | (sequence & 0xFFFFFFu);
    }
    [[nodiscard]] constexpr PlayerId SharedIdOwner(std::uint32_t sharedId) noexcept { return static_cast<PlayerId>(sharedId >> 24); }
    [[nodiscard]] bool IsValidDroppedItem(const DroppedItem& d) noexcept;

    using Message = std::variant<Hello, Welcome, Reject, SessionSnapshot, PlayerJoined, PlayerLeft,
                                 Heartbeat, HeartbeatAck, Disconnect, PlayerTransform, NpcCombat, GroupRequest,
                                 GroupState, NpcAuthority, NpcStates, NpcDeath, NpcDamage, NpcEvents, NpcOutfits,
                                 WorldEvents, WorldClock, DialogueLine, DroppedItem>;

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
    [[nodiscard]] bool IsValidControls(const PlayerControls& c) noexcept;
    [[nodiscard]] bool IsValidNpcState(const NpcState& s) noexcept;

    // Serial-number comparison (RFC 1982 style) for wrapping u32 sequences.
    [[nodiscard]] constexpr bool SequenceNewer(std::uint32_t candidate, std::uint32_t last) noexcept
    {
        return candidate != last && static_cast<std::int32_t>(candidate - last) > 0;
    }
}
