#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Animation graph events (Starfield.exe 1.16.244; evidence in docs/REVERSE_ENGINEERING.md §8).
//
// Capture: the engine delivers every animation graph event of an actor to the actor's own
// BSTEventSink<BSAnimationGraphEvent> (TESObjectREFR subobject at +0x50). PlayerCharacter's
// vtable for that subobject is Address Library ID 452451 (RVA 0x4CBC3D0); slot 1 is
// ProcessEvent (0x18F1490, shared with Actor). The event carries its tag (BSFixedString)
// at +0x08 and its payload (BSFixedString) at +0x10. That layout is read from the handler.
//
// Replay: IAnimationGraphManagerHolder at TESObjectREFR+0x60, vfunc 1, called as
// (holder, const BSFixedString& event, nullptr). That is exactly how ObjectReference.PlayAnimation
// (native 0x207C1E0) notifies non-actor graphs; for actors that native refuses ("is an
// Actor, and PlayAnimation should not be used."), so the plugin calls the vfunc itself.
namespace cosync::plugin::animevents
{
    struct CapturedEvent
    {
        std::string tag;
        std::string payload;
        bool input = false;     // true: an event the engine SENT TO the player's graph (Notify)
        bool accepted = false;  // input only: the graph accepted it (Notify returned true)
    };

    // Hooks the local player's animation-event sink (once). Load/main thread.
    bool InstallPlayerCapture(std::string& detail);

    // Hooks NotifyAnimationGraph (IAnimationGraphManagerHolder slot 1) on the PlayerCharacter
    // holder vtable (ID 452463; the function 0xF09F60 is shared with Actor, but only the player's
    // vtable slot is patched). Captures every event the engine sends INTO the player's graph:
    // the inputs that start actions (jump, sneak, attacks, ...). Load/main thread.
    bool InstallPlayerInputCapture(std::string& detail);

    // Moves captured events (oldest first) into `out`. Thread-safe. `dropped` counts
    // events lost because the buffer was full.
    void Drain(std::vector<CapturedEvent>& out, std::uint64_t& dropped);

    // Sends an animation graph event to the actor with this FormID. MAIN THREAD ONLY.
    bool Notify(std::uint32_t refId, const char* eventName);
}
