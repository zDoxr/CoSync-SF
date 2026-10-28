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
//   ObjectReference PlaceAtMe(Form, Int, Bool, Bool, Bool, Float[], alias, Bool)
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
    inline constexpr std::uint32_t kSpawnReturnedNone = 0xFFFFFFF1;  // PlaceAtMe returned None
    inline constexpr std::uint32_t kSpawnWrongType = 0xFFFFFFF2;     // result was not an object
    inline constexpr std::uint32_t kSpawnUnpackFailed = 0xFFFFFFF3;  // object, but no reference behind it
    [[nodiscard]] constexpr bool IsSpawnError(std::uint32_t v) noexcept { return v >= kSpawnCanceled; }
    [[nodiscard]] const char* DescribeSpawnError(std::uint32_t v) noexcept;

    enum class SpawnBase
    {
        PlayerBase,  // the local player's own base NPC
        NearbyNpc,   // the base NPC of a living, non-player actor in the player's cell
    };
    // Places a copy of the chosen base NPC at the local player. When the VM
    // finishes, *result is set to the new reference's FormID or to one of the
    // kSpawn* error codes. Returns false and sets `error` if the call could not
    // be dispatched; `detail` describes the base form used (for logs).
    bool RequestSpawn(SpawnBase which, std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error,
                      std::string& detail);

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
