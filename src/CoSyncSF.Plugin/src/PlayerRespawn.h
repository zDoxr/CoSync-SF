#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Dying in a session must not reload a save: a reload rewinds one copy against all the others. So
// during a session the local player cannot die. The killing blow is stopped at the engine's kill
// dispatcher and the player's death slots, and the player returns to their last safe spot at full health. Copies that do not own
// the session's world also cannot save. Evidence: docs/REVERSE_ENGINEERING.md 12.13.
namespace cosync::plugin::respawn
{
    struct Context
    {
        bool inSession = false;
        bool inWorld = false;      // the player is in the world and the script VM is running
        bool ownsWorld = false;    // this copy's saves are the session's world: the NPC authority
        std::vector<std::uint32_t> standIns;  // other players' stand-ins loaded here
    };

    // Hooks the kill dispatcher. Nothing changes until a session is running. Call once, at load.
    bool Install(std::string& detail);
    // Main thread, each frame while a session exists.
    void Tick(const Context& context);
    // The session ended: saving is allowed again and the checkpoint marker is removed. Main thread.
    void Leave();
    // '@killself', '@killselfhit', '@cansave', '@savetest', '@checkpoint', '@checkpointhold 0|1'.
    bool RunStep(const std::string& step);
}
