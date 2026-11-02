#pragma once

#include "cosync/shared/Types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// Remote-player stand-ins ("avatars") via the game's Papyrus VM.
//
// CommonLibSF exposes no native reference-creation function, so placement
// goes through Papyrus. Signatures were read from the game's compiled scripts
// (Starfield - Misc.ba2, scripts/objectreference.pex and actor.pex, 1.16.244):
//   ObjectReference PlaceAtMe(Form, Int, Bool, Bool, Bool, Float[], alias, Bool)  (rejected: see Avatars.cpp)
//   actor PlaceActorAtMe(actorbase, Int, location, Bool, Bool, Bool, Float[], Bool)  (used)
//   None SetPosition(Float, Float, Float)      None SetAngle(Float, Float, Float)  (degrees)
//   None EnableAI(Bool abEnable, Bool abPauseVoice)
//   None Disable(Bool abFadeOut)               None Delete()
// Calls are queued on the VM and complete asynchronously. All functions here
// must be called on the MAIN THREAD; the spawn result arrives on a VM thread
// and is published through the atomic.
namespace cosync::plugin::avatars
{
    // Spawn results that are not a FormID (diagnostic detail for logs).
    inline constexpr std::uint32_t kSpawnCanceled = 0xFFFFFFF0;      // VM cancelled the call
    inline constexpr std::uint32_t kSpawnReturnedNone = 0xFFFFFFF1;  // placement returned None
    inline constexpr std::uint32_t kSpawnWrongType = 0xFFFFFFF2;     // result was not an object
    inline constexpr std::uint32_t kSpawnUnpackFailed = 0xFFFFFFF3;  // object, but no reference behind it
    [[nodiscard]] constexpr bool IsSpawnError(std::uint32_t v) noexcept { return v >= kSpawnCanceled; }
    [[nodiscard]] const char* DescribeSpawnError(std::uint32_t v) noexcept;

    // Radius for picking a template actor near the player (engine units).
    inline constexpr float kTemplateSearchRadius = 4096.0f;

    [[nodiscard]] std::uint32_t PlayerRefId();

    // Asks the VM for a random actor near the local player (Game.FindRandomActor).
    // *result receives that actor's FormID, kSpawnReturnedNone if there is none,
    // or another kSpawn* error. It may be the player itself; callers retry.
    bool RequestTemplateActor(std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error);

    // Asks the VM for an actor's base as scripts see it (ObjectReference.GetBaseObject).
    // In memory, loaded NPCs carry bases generated at runtime (FFxxxxxx), and
    // PlaceActorAtMe returns None for those (2026-10-27). *result receives the
    // base's FormID if it is an NPC_, else a kSpawn* error.
    bool RequestBaseObject(std::uint32_t actorId, std::shared_ptr<std::atomic<std::uint32_t>> result,
                           std::string& error);

    // True if `baseId` is an NPC_ defined in a data file (not generated at runtime).
    [[nodiscard]] bool IsPlaceableBase(std::uint32_t baseId);

    // Places an actor of base NPC `baseNpcId` (0, or not placeable, falls back to
    // the player's base) at the local player. When the VM finishes, *result is
    // set to the new reference's FormID or a kSpawn* error code. Returns false
    // and sets `error` if it could not be dispatched; `detail` describes the base.
    bool RequestSpawn(std::uint32_t baseNpcId, std::shared_ptr<std::atomic<std::uint32_t>> result,
                      std::string& error, std::string& detail);

    // True if the Papyrus VM is running normally. During a save load the VM is
    // frozen (Papyrus.0.log: "VM is freezing" ... "VM is thawing") and calls
    // dispatched then are dropped without a reply (observed 2026-10-27).
    // Reads BSScript::Internal::VirtualMachine::freezeState and ::overstressed.
    [[nodiscard]] bool VmReady();

    // Stops the avatar's AI so the game does not steer it. False if the ref is gone.
    bool Freeze(std::uint32_t refId);

    // Moves the avatar (Papyrus SetPosition/SetAngle). Rotation in radians.
    // False if the reference no longer exists.
    bool Move(std::uint32_t refId, const Vec3& position, const Vec3& rotationRadians);

    // Papyrus ObjectReference.SetScale(Float) (1 param, registered in the exe). Debug aid.
    bool SetScale(std::uint32_t refId, float scale);

    // Disables and deletes the avatar. Safe to call with a stale id.
    void Despawn(std::uint32_t refId);

    // Engine-side state of a spawned avatar, read synchronously on the MAIN THREAD.
    // Every read is backed by disassembly of Starfield.exe 1.16.244 (docs/REVERSE_ENGINEERING.md):
    //   form type   TESForm+0x2E == 0x4B (ACHR)       PlaceActorAtMe native 0x207A220 compares it
    //   disabled    TESForm+0x20 bit 11              ObjectReference.IsDisabled native 0x208BAB0
    //   deleted     TESForm+0x20 bit 5               CommonLibSF kDeleted (not yet traced in the exe)
    //   parent cell TESObjectREFR+0xB0               PlaceActorAtMe passes it to vfunc 0x10C (SetParentCell)
    //   3D          vtbl[0xAC](NiPointer<NiAVObject>&) ObjectReference.Is3DLoaded native 0x2075F00
    struct ProxyState
    {
        bool exists = false;        // LookupByID found a form
        bool isActor = false;       // form type ACHR
        bool disabled = false;
        bool deleted = false;
        bool has3D = false;         // Get3D returned a scene-graph node
        bool sameCellAsPlayer = false;
        std::uint32_t parentCellId = 0;
        std::uint32_t baseId = 0;
        Vec3 position;
        float distanceToPlayer = -1.0f;
    };
    [[nodiscard]] ProxyState Probe(std::uint32_t refId);
    [[nodiscard]] std::string Describe(const ProxyState& s);
}
