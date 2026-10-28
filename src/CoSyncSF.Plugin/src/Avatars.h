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
    // Places a copy of the local player's base NPC at the local player. When the
    // VM finishes, *result is set to the new reference's FormID (or to
    // kSpawnFailed). Returns false and sets `error` if the call could not be dispatched.
    inline constexpr std::uint32_t kSpawnFailed = 0xFFFFFFFF;
    bool RequestSpawn(std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error);

    // Stops the avatar's AI so the game does not steer it. False if the ref is gone.
    bool Freeze(std::uint32_t refId);

    // Moves the avatar (Papyrus SetPosition/SetAngle). Rotation in radians.
    // False if the reference no longer exists.
    bool Move(std::uint32_t refId, const Vec3& position, const Vec3& rotationRadians);

    // Disables and deletes the avatar. Safe to call with a stale id.
    void Despawn(std::uint32_t refId);
}
