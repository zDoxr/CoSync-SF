#pragma once

#include "cosync/shared/Types.h"

#include <cstdint>
#include <string>
#include <vector>

// NPC synchronisation (docs/NPC_SYNC_ARCHITECTURE.md). Step 1: enumerate the actors the engine
// currently runs in its high process list, with a runtime check of the layout.
namespace cosync::plugin::npcsync
{
    struct ActorInfo
    {
        std::uint32_t formId = 0;
        std::uint32_t baseId = 0;
        Vec3 position;
        float yaw = 0.0f;
    };

    struct Enumeration
    {
        bool ok = false;           // layout check passed
        std::string check;         // human-readable verdict
        std::uint32_t arraySize = 0;
        std::uint32_t unresolved = 0;  // handles that did not resolve to an ACHR
        std::vector<ActorInfo> actors;
    };

    // The engine's loaded-actor list in ProcessLists (singleton ID 937584): u32 count at +0x60,
    // u32 handle array at +0x68, as walked by 0x1A9A1A0 (Game.FindRandomActor). Every handle must
    // resolve to an Actor. Skips the player. Reads only verified fields. MAIN THREAD ONLY.
    Enumeration EnumerateHighActors();
}
