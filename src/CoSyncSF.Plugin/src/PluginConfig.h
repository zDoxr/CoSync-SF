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

    std::filesystem::path ThisModuleDirectory();
    std::filesystem::path LogDirectory();  // Documents\My Games\Starfield\SFSE\Logs
}
