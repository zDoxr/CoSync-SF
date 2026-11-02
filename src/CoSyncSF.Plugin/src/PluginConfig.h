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
        bool avatarAnimDiag = false;  // [Experimental] AvatarAnimDiag: log graph variables 1/s
        bool avatarAnimate = false;   // [Experimental] AvatarAnimate: drive locomotion variables
        bool avatarMove = true;       // [Experimental] AvatarMove (SetPosition/SetAngle at 10 Hz)
        float avatarScale = 1.0f;     // [Experimental] AvatarScale (debug: >1 makes proxies easy to spot)
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
