#pragma once

// CoSync-SF multiplayer UI core: the contract between the multiplayer
// interface (Scaleform menu, overlays, an external launcher) and the plugin.
//
// It has no engine or rendering code, so it is unit-tested. See docs/UI_FRAMEWORK.md §3:
//   view  --(intent name + string arguments)-->  ParseIntent  -->  UiCommand (validated)
//   UiModel  --Build()-->  UiSnapshot (immutable, versioned)  -->  ToJson  -->  view
// The view never calls engine code or sockets. UiCommands are executed by the
// plugin on the main thread (session actions go to NetworkWorker, game actions
// to GameBridge).

#include "cosync/net/SessionClient.h"
#include "cosync/shared/Types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cosync::ui
{
    // ---- Commands (view -> core) ---------------------------------------------
    enum class Tab : std::uint8_t { Session, Browser, Players, Diagnostics, Settings };

    namespace cmd
    {
        struct HostGame { std::string serverName; std::uint16_t port = 37015; std::uint16_t maxPlayers = 8; std::string password; };
        struct JoinGame { std::string address; std::string password; };  // "ip:port" (validated)
        struct LeaveGame {};
        struct KickPlayer { PlayerId player = kInvalidPlayerId; };
        struct SetDisplayName { std::string name; };
        struct RefreshServers {};
        struct SelectTab { Tab tab = Tab::Session; };
        struct SetTheme { std::string name; };
    }
    using UiCommand = std::variant<cmd::HostGame, cmd::JoinGame, cmd::LeaveGame, cmd::KickPlayer, cmd::SetDisplayName,
                                   cmd::RefreshServers, cmd::SelectTab, cmd::SetTheme>;

    // Arguments arrive from the view as strings (Scaleform/AS3, or any other front end).
    using IntentArgs = std::map<std::string, std::string, std::less<>>;

    // Validates an intent. On failure returns nullopt and a user-facing reason.
    [[nodiscard]] std::optional<UiCommand> ParseIntent(std::string_view intent, const IntentArgs& args,
                                                       std::string& error);
    [[nodiscard]] std::string_view ToString(Tab tab) noexcept;

    // ---- Theme ----------------------------------------------------------------
    // Visual tokens pushed to the view, so the look can change without rebuilding the movie.
    struct Theme
    {
        std::string name = "Starfield Modern";
        std::uint32_t background = 0x0B0F14;   // 0xRRGGBB
        std::uint32_t panel = 0x141B24;
        std::uint32_t panelAlt = 0x1B2430;
        std::uint32_t text = 0xE8ECEF;
        std::uint32_t textDim = 0x8A96A3;
        std::uint32_t accent = 0xE8A33D;       // amber
        std::uint32_t accentAlt = 0x4FB3D9;    // cyan
        std::uint32_t good = 0x5CC98A;
        std::uint32_t warn = 0xE8C33D;
        std::uint32_t bad = 0xE5534B;
        float panelOpacity = 0.92f;
        float cornerRadius = 2.0f;
        float spacing = 12.0f;                 // base spacing unit, px at 1080p
        float fontScale = 1.0f;
        std::uint32_t transitionMs = 180;
        bool operator==(const Theme&) const = default;
    };
    // Parses "key = value" lines ('#'/';' comments; colours as #RRGGBB). Unknown keys
    // and bad values are reported in `warnings` and leave the defaults in place.
    [[nodiscard]] Theme ParseTheme(std::string_view text, std::string& warnings);

    // ---- Snapshot (core -> view) ----------------------------------------------
    struct PlayerRow
    {
        PlayerId id = kInvalidPlayerId;
        std::string name;
        bool isLocal = false;
        bool isHost = false;
        bool sameLocation = false;
        std::string proxy;  // "none", "spawning", "valid", "incomplete"
        bool operator==(const PlayerRow&) const = default;
    };

    struct ServerRow
    {
        std::string name;
        std::string address;
        std::uint16_t players = 0;
        std::uint16_t maxPlayers = 0;
        std::uint32_t pingMs = 0;
        bool passworded = false;
        std::string gameVersion;
        bool operator==(const ServerRow&) const = default;
    };

    struct UiSnapshot
    {
        std::uint64_t version = 0;
        Tab tab = Tab::Session;
        // Connection
        net::ClientState state = net::ClientState::Idle;
        std::string stateDetail;
        bool hosting = false;
        std::string serverName;
        std::string address;
        PlayerId localPlayer = kInvalidPlayerId;
        std::uint32_t rttMs = 0;
        std::uint64_t droppedEvents = 0;
        std::string protocolVersion;
        std::string gameVersion;
        // Lists
        std::vector<PlayerRow> players;  // includes the local player
        std::vector<ServerRow> servers;
        std::vector<std::string> diagnostics;  // newest last, bounded
        // Settings
        std::string displayName;
        Theme theme;
        std::string lastError;  // most recent rejected intent / failure, for a toast
    };

    // Serializes a snapshot for the view (strict JSON, UTF-8 passed through).
    [[nodiscard]] std::string ToJson(const UiSnapshot& s);

    // ---- Model ------------------------------------------------------------------
    // Owns UI-only state and assembles snapshots. Not thread-safe: use from one thread.
    class UiModel
    {
    public:
        static constexpr std::size_t kMaxDiagnostics = 64;

        // Inputs from the plugin.
        void SetConnection(net::ClientState state, std::string detail, bool hosting, std::string serverName,
                           std::string address, PlayerId localPlayer, std::uint32_t rttMs, std::uint64_t dropped);
        void SetVersions(std::string protocolVersion, std::string gameVersion);
        void SetPlayers(std::vector<PlayerRow> players);
        void SetServers(std::vector<ServerRow> servers);
        void AddDiagnostic(std::string line);
        void SetDisplayName(std::string name);
        void SetTheme(Theme theme);
        void SetError(std::string error);

        // Applies UI-only commands (tab selection) locally and returns true; returns
        // false for commands the plugin must execute (session actions, theme loading).
        bool ApplyLocal(const UiCommand& command);

        // Current snapshot; the version increases only when content changed.
        [[nodiscard]] const UiSnapshot& Snapshot() const noexcept { return _snapshot; }

    private:
        void Touch() noexcept { ++_snapshot.version; }
        UiSnapshot _snapshot;
    };
}
