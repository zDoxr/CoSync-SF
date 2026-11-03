#pragma once

#include <cstdint>
#include <string>

// Runtime session control for the in-game UI (main thread only). Implemented in Plugin.cpp,
// which owns the network worker.
namespace cosync::plugin::session
{
    // Stops the current session (leaves gracefully, despawns proxies). Safe when idle.
    void Leave();
    // Leaves any current session, then hosts. False with a reason on failure.
    bool Host(const std::string& serverName, std::uint16_t port, std::uint16_t maxPlayers,
              const std::string& password, std::string& error);
    // Leaves any current session, then joins "ip:port".
    bool Join(const std::string& address, const std::string& password, std::string& error);
}
