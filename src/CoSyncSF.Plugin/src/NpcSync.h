#pragma once

#include "cosync/shared/NpcReplication.h"
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
        bool hasCrowdTarget = false;  // crowd actors with a target only
        Vec3 crowdTarget;             // that target's position
    };

    // The ambient crowd (RE 12.10): real actors, but CrowdManager's own, each copy spawning its own.
    std::vector<ActorInfo> EnumerateCrowdActors();

    // CrowdManager's walk, sit and stand targets as this copy knows them (RE 12.10).
    std::vector<npc::CrowdTarget> EnumerateCrowdTargets();

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

    // Actors with 3D in the other ProcessLists arrays (+0x70, +0x90, +0x190; found at runtime,
    // 2026-11-04) that the +0x60 list does not hold. +0x70 has over a thousand entries, so this is
    // for refreshing about once a second, not every frame. MAIN THREAD ONLY.
    std::vector<ActorInfo> EnumerateOtherLoadedActors();

    // Diagnostic: every {u32 count, ptr} pair in the first 0x200 bytes of ProcessLists whose entries
    // resolve as actor handles. Shows whether the engine keeps actors in other lists. Read-only.
    std::string ScanActorLists();
}
