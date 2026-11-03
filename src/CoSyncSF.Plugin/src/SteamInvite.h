#pragma once

#include <cstdint>
#include <string>

// Steam friends integration through the game's own steam_api64.dll (flat API, resolved at
// runtime; no SDK dependency). MAIN THREAD.
namespace cosync::plugin::steam
{
    // steam_api64.dll is loaded and the friends interface is available.
    bool Available();
    // Opens the Steam overlay's invite dialog; invited friends receive `connectString`.
    bool OpenInviteDialog(const std::string& connectString);
    // Rich presence "connect" (Steam's "Join Game" for friends). Empty clears it.
    bool SetJoinPresence(const std::string& connectString);
    // Primary local IPv4 address ("" if unknown): the interface the OS would route to the
    // internet with. No packets are sent.
    std::string LocalIPv4();
    // This user's SteamID64 (0 if Steam is unavailable).
    std::uint64_t LocalSteamId();
    // Join code that works across the internet through Steam's relays: "steam:<SteamID64>".
    std::string SteamJoinCode();
    // Listens for accepted invites / "Join Game" (GameRichPresenceJoinRequested_t, callback 337,
    // dispatched by the game's own SteamAPI_RunCallbacks) and reads a +cosync_join argument the
    // game was launched with. Install once (main thread, after Steam is up).
    void InstallInviteListener();
    // The join code of the newest accepted invite, once (thread-safe).
    std::string TakePendingJoin();
    // Puts text on the Windows clipboard.
    bool CopyToClipboard(const std::string& text);
}
