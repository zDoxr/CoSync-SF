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

    // Places an actor with the base NPC of `templateActorId` (0, or a non-NPC,
    // falls back to the player's base) at the local player. When the VM finishes,
    // *result is set to the new reference's FormID or a kSpawn* error code.
    // Returns false and sets `error` if it could not be dispatched; `detail`
    // describes the base used (for logs).
    bool RequestSpawn(std::uint32_t templateActorId, std::shared_ptr<std::atomic<std::uint32_t>> result,
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

    // Disables and deletes the avatar. Safe to call with a stale id.
    void Despawn(std::uint32_t refId);
}
