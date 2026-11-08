#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Player-controls input replication (Starfield.exe 1.16.244; evidence in
// docs/ANIMATION_REVERSE_ENGINEERING.md §A.10).
//
// The player's movement agent (MovementHandlerAgentPlayerControls) asks its controller's
// interface registry for "IMovementPlayerControls" every frame and calls its vfunc 1 as
// `(iface, actorFormID, ControlsData* out)`. The engine's implementation is
// PlayerControls::Manager+0x20 (vtable ID 433843, fill = 0x12C5E60), which turns the
// player's input into a movement request; the controller then moves the actor and the
// animation graph receives Speed/Direction from that request.
//
// Capture: hook that fill and keep a copy of what it produced for the local player.
// Replay:  register a CoSync IMovementPlayerControls on a proxy's controller registry
//          (Actor+0x240 -> +0x48) that serves a stored ControlsData.
namespace cosync::plugin::movecontrols
{
    // Bytes of ControlsData copied: every field the engine's producers write lies below
    // +0x58 (+0x54 is the last, a byte). The caller's buffer is larger.
    constexpr std::size_t kControlsSize = 0x58;

    struct Controls
    {
        std::byte data[kControlsSize]{};
        std::uint64_t seq = 0;  // 0 = never captured
    };

    // Hooks the engine's fill (once). Load/main thread.
    bool InstallCapture(std::string& detail);
    // Latest ControlsData the engine produced for the local player (thread-safe).
    Controls LatestPlayerControls();
    // Short human-readable view of the known fields.
    std::string Describe(const Controls& c);

    // Experiment "mirror": the proxy's controller serves the local player's latest captured
    // ControlsData (no networking). Replaces any registered IMovementPlayerControls. MAIN THREAD.
    bool AttachMirror(std::uint32_t refId, std::string& detail);

    // Switches the actor's MovementControllerNPC into movement state 1, the state the engine uses
    // for the player (its config lists the "Player Controls" agents; RE A.10/A.11), by calling the
    // controller's own vfunc 9 (0x1AB7EB0, ID 103568). Refuses unless the vtable slot holds exactly
    // that function. MAIN THREAD. Experimental.
    bool EnterPlayerControlsState(std::uint32_t refId, std::string& detail);

    // ---- Network replay (AvatarMoveMode=controls) -----------------------------
    // One static IMovementPlayerControls object per remote player slot (never freed, so the
    // engine can never hold a dangling pointer). Slot = PlayerId % kReplaySlots.
    constexpr std::size_t kReplaySlots = 64;
    // A separate pool for NPCs: slot ids kNpcSlotBase + 0..kNpcReplaySlots-1.
    constexpr std::size_t kNpcReplaySlots = 256;
    constexpr std::uint32_t kNpcSlotBase = 0x10000000;
    // Registers player `slotId`'s replay object on the actor's controller (replacing any
    // IMovementPlayerControls). MAIN THREAD.
    bool AttachReplay(std::uint32_t refId, std::uint32_t slotId, std::string& detail);
    // Data the replay object serves from now on; active=false serves no input (the caller's
    // default buffer, i.e. stand still). Thread-safe.
    void SetReplay(std::uint32_t slotId, const Controls& c, bool active);
    // Yaw delta for the replay object to apply on the engine's next read (consumed once), and the
    // heading that results. Thread-safe; call once per game frame.
    void SetReplayTurn(std::uint32_t slotId, float yawDelta, float heading);
    // The engine's jump for an actor, 0x18E53E0 (ID 100537) `bool (Actor*, float height)`, called with
    // the height its own JumpAnimEventHandler passes (global ID 500287, 1.14 in 1.16.244). Sets the
    // character controller's jump height and starts the jump (RE A.12). MAIN THREAD.
    bool Jump(std::uint32_t refId, std::string* detail = nullptr);
    // Lets Jump() pass the NPC combat-controller check for the proxy being jumped (Actor vfunc 363;
    // RE A.12). Install at plugin load.
    void InstallJumpHook(std::string& detail);
    // Closed-loop movement input for the replay object: +0x08 direction relative to the proxy's
    // facing (atan2(right, forward), radians) and +0x30 magnitude (0 stand .. 2 run). Overrides the
    // sender's values. Thread-safe.
    void SetReplayMove(std::uint32_t slotId, float angle, float magnitude);
    // NPC aim: the graph variables DirectAtPitchVar / DirectAtHeadingVar / bAimActive are bound to
    // the actor's AI process (process->+0x08: +0x530 / +0x52C / +0x587; RE A.4), so the graph reads
    // these fields directly. Writes pitch (radians), heading offset 0 and aimActive. MAIN THREAD.
    bool SetAim(std::uint32_t refId, float pitch, bool aimActive);
    // Removes every entry for `refId` (and any dead entry) from PlayerControls::Manager's list of
    // controlled actors (Manager+0x20 interface slot 3 = 0x12C6440, the engine's own remove). A
    // proxy's player-controls agent may have registered there; once the proxy is deleted the
    // engine's input dispatch 0x12C6F10 dereferences the dead entry and crashes (2026-11-03 dump:
    // Starfield.exe+0x12C70D8 reading null+0x2E). Call before despawning a proxy. Returns the count
    // removed. MAIN THREAD.
    int PurgeFromPlayerControls(std::uint32_t refId);
    // Hooks MovementControllerNPC's state-changing vfuncs (slots 7, 8, 10..14; RE 12.3). A held
    // controller refuses them, so an NPC's own AI cannot pull it out of the player-controls state.
    // Every other controller passes straight through. Install at plugin load.
    void InstallControllerHold(std::string& detail);
    // Holds or frees the actor's controller. MAIN THREAD.
    bool SetControllerHeld(std::uint32_t refId, bool held);
    // State changes refused so far, by requested slot (index 0..15).
    std::uint64_t HoldRefusals(std::size_t slot);

    // MovementControllerNPC state (+0xC0) of the actor, or -1. MAIN THREAD.
    int ControllerState(std::uint32_t refId);
    // MovementControllerAI mode (+0x5C) of the actor: 2 motion-driven, 1 animation-driven, -1 none.
    int ControllerMode(std::uint32_t refId);
    // Undoes AttachReplay + EnterPlayerControlsState: the controller's own vfunc 7 (0x1AB7D80,
    // ID 103566) reselects the default state, the replay object goes, and a motion-driven controller
    // gets the engine's IMovementPlayerControls back, as its vfunc 16 registered it. MAIN THREAD.
    bool LeavePlayerControlsState(std::uint32_t refId, std::string& detail);

    // Which movement interfaces the actor's controller registry holds (diagnostics). MAIN THREAD.
    std::string DescribeRegistry(std::uint32_t refId);
}
