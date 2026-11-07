#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Unattended in-game testing. Off unless [Dev] keys ask for it (scripts/launch-instances.ps1).
namespace cosync::plugin::dev
{
    struct Options
    {
        std::string autoLoad;               // [Dev] AutoLoad: save loaded from the main menu
        bool eventCensus = false;           // [Dev] EventCensus: counting sink on every global event source
        std::vector<std::string> commands;  // [Dev] TestCommands: console commands, ';' separated
        std::uint32_t commandDelay = 30;    // [Dev] TestCommandDelay: seconds in the world before the first
    };

    void Configure(Options options);

    // The engine's console entry (0x1EA1E10). Main thread. The text is a format string to the
    // engine, so '%' is escaped here.
    void RunConsole(const std::string& command);

    // Main thread, every frame, from the main menu on.
    void Tick(bool inWorld);

    // Carries out '@name' test steps. Main thread.
    void SetCommandHandler(std::function<bool(const std::string&)> handler);
}
