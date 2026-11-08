#pragma once

#include <cstdint>

// Unattended measurement for test runs, [Dev] SelfTest=N (off by default).
//
// N seconds after both the session and the world are up, opens each menu that pauses the game in
// turn and logs, per menu, whether the engine reports a pause and how far NPCs moved while it was
// open. The numbers show whether the world keeps running, without anyone at the keyboard.
namespace cosync::plugin::selftest
{
    void Configure(std::uint32_t delaySeconds) noexcept;
    // Main thread. `ready`: in a session and in the world.
    void Tick(bool ready);
}
