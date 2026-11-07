#pragma once

#include "cosync/shared/Log.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace cosync::plugin
{
    enum class Mode
    {
        Off,
        Host,
        Client,
    };

    struct Config
    {
        Mode mode = Mode::Off;
        std::string address = "127.0.0.1:37015";
        std::uint16_t port = 37015;
        std::uint16_t discoveryPort = 37016;
        std::string serverName = "CoSync-SF Session";
        std::uint16_t maxPlayers = 8;
        std::string playerName = "Player";
        std::string password;
        log::Level logLevel = log::Level::Info;
        bool remoteAvatars = false;  // [Experimental] RemoteAvatars
        // [Experimental] AvatarFreezeAI: 0 = never; 1 = EnableAI(false) right after spawn (left
        // proxies stuck semi-transparent in game, 2026-11-02); 2 = after 3D has been loaded for
        // AvatarFreezeDelay seconds.
        int avatarFreezeMode = 0;
        float avatarFreezeDelay = 3.0f;
        // [Experimental] AvatarBase: "player" = the player character's own base NPC (00000007);
        // "nearby" = the data-file base of a random nearby NPC (old behaviour).
        bool avatarNearbyBase = false;
        // [Experimental] AvatarAnimDiag: 0 off; 1 = native animation probe, transition-based
        // (player + proxies) and player animation events; 2 = also float variables at 2 Hz.
        int avatarAnimDiag = 0;
        bool avatarAnimate = false;   // [Experimental] AvatarAnimate: drive locomotion variables
        bool avatarMove = true;       // [Experimental] AvatarMove (SetPosition/SetAngle at 10 Hz)
        bool avatarPath = false;      // [Experimental] AvatarMoveMode=path: engine pathing to a marker
        bool avatarMotion = false;    // [Experimental] AvatarMoveMode=motion: motion-driven controller + teleport
        bool avatarMirror = false;    // [Experimental] AvatarMoveMode=mirror: proxies replay the local player's controls
        bool avatarControls = false;  // [Experimental] AvatarMoveMode=controls: proxies replay the remote player's controls
        bool npcSync = false;         // [Experimental] NpcSync: replicate NPCs (docs/NPC_SYNC_STATUS.md)
        float avatarScale = 1.0f;     // [Experimental] AvatarScale (debug: >1 makes proxies easy to spot)
        // [Dev] FakeSteamId: 0 = real Steam; N (1..999) = test identity steam:N over local GNS (several
        // copies on one PC with one Steam account). Invites go through a shared file in %TEMP%.
        std::uint32_t fakeSteamId = 0;
        // [Dev] SelfTest: 0 = off; N = seconds in the world before the unattended menu test.
        std::uint32_t selfTest = 0;
        // [Dev] MenuPatch=0: menus pause as in the unmodified game (control runs).
        bool menuPatch = true;
    };

    // Reads the INI; missing keys keep their defaults, invalid values are
    // reported in `warnings` and also keep their defaults.
    Config LoadConfig(const std::filesystem::path& iniPath, std::string& warnings);

    // Per-process overrides so several game instances on one PC can share one
    // Data folder (see scripts/launch-instances.ps1):
    //   COSYNC_INI      - full path of the config file to use instead of the default
    //   COSYNC_INSTANCE - short label appended to the log file name (CoSyncSF-<label>.log)
    std::filesystem::path ConfigPath();
    std::wstring LogFileName();

    // <plugin dir>/versionlib-<major>-<minor>-<build>-0.bin, the file
    // CommonLibSF's REL::IDDB loads (commonlib-shared src/REL/IDDB.cpp).
    std::filesystem::path AddressLibraryPath(std::uint32_t packedRuntime);

    std::filesystem::path ThisModuleDirectory();
    std::filesystem::path LogDirectory();  // Documents\My Games\Starfield\SFSE\Logs
}
