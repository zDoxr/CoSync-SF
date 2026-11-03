#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Engine-native, read-only animation/movement probe (Starfield.exe 1.16.244).
// Evidence for every offset and slot: docs/ANIMATION_REVERSE_ENGINEERING.md.
//
// Reads, for one actor, on the MAIN THREAD only (the actor is looked up by FormID on each call):
//   * Actor::currentProcess (+0x228) and its byte +0xE7 (selects animation- vs motion-driven
//     movement in the engine's own controller set-up, RE A.5)
//   * Actor::movementController (+0x240, MovementControllerNPC): mode byte +0x5C
//     (1 = IMovementMotionDrivenControl absent, 2 = present), configuration state +0xC0,
//     byte +0x70
//   * animation graph variables through IAnimationGraphManagerHolder (Actor+0x60)
//     vfuncs 0x12/0x13/0x14 (float/int/bool getters), the same calls the engine makes
//
// Raw memory reads are guarded with SEH so a wrong layout fails as "unreadable", not a crash.
namespace cosync::plugin::animprobe
{
    enum class Kind : std::uint8_t { kFloat, kInt, kBool };

    struct VarSpec
    {
        const char* name;
        Kind kind;
    };

    // Variables read by the probe (all present as names in the exe; see the RE document).
    const std::vector<VarSpec>& Vars();

    struct VarValue
    {
        bool ok = false;  // getter returned true (variable exists on this graph)
        float f = 0.0f;
        std::int32_t i = 0;
        bool b = false;
    };

    struct Snapshot
    {
        bool found = false;      // FormID resolves to an Actor (ACHR)
        bool isPlayerClass = false;  // primary vtable is PlayerCharacter's
        bool readable = true;    // no SEH fault while reading
        std::uintptr_t process = 0;
        std::uintptr_t controller = 0;
        int processByteE7 = -1;
        int controllerMode = -1;     // +0x5C
        int controllerConfig = -1;   // +0xC0
        int controllerByte70 = -1;   // +0x70
        bool graphPresent = false;   // at least one variable readable
        std::vector<VarValue> values;  // parallel to Vars()
    };

    // MAIN THREAD ONLY.
    Snapshot Read(std::uint32_t refId);
    // One float graph variable through the holder getter (slot 0x12). MAIN THREAD ONLY.
    bool ReadFloat(std::uint32_t refId, const char* name, float& out);

    // One line with the movement-controller state and every readable discrete variable.
    std::string DescribeState(const Snapshot& s);
    // Floats only (Speed, Direction, ...).
    std::string DescribeFloats(const Snapshot& s);
    // Changes of controller state and of int/bool variables between two snapshots
    // ("" if nothing discrete changed). Floats are not compared.
    std::string DescribeTransitions(const Snapshot& before, const Snapshot& after);
    // True if the float values moved by more than eps since `before`.
    bool FloatsChanged(const Snapshot& before, const Snapshot& after, float eps);

    // Validates the int/bool getter slots against values the engine sets on the player's graph
    // (IsPlayer=true, IsNPC=false, iIsPlayer=1; RE A.2). Returns a human-readable verdict.
    std::string SelfCheckPlayer(const Snapshot& player, bool& passed);

    // ---- Movement controller experiment (AvatarMoveMode=motion) ---------------
    // Calls the engine's own movement-controller (re)initialisation 0x1922740 (ID 100903),
    // `(Actor*, bool motionDriven)`, exactly as the engine does when an actor's process changes
    // (caller 0xB4A9C0 passes `process+0xE7 == 0`). MAIN THREAD ONLY. Experimental.
    bool ReinitMovementController(std::uint32_t refId, bool motionDriven, std::string& detail);
}
