# CoSync-SF — Remote Player Proxy Research

How a remote player should exist in Starfield's world, why earlier attempts misbehaved, and what is implemented now. All evidence references are to [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) (RE §n) and to in-game logs.

## Status (2026-11-02)

**First milestone reached: a remote player is represented by a valid, visible actor inside the world.** Two Starfield copies on one PC, connected through a CoSync session, each spawned a proxy for the other player. The plugin validated each proxy through engine state:

```
[avatar] player 2 'Player2': spawned ref FF01799E (...); right after spawn: Actor enabled 3D=NONE cell=000125B6 (player's cell) ...
[avatar] player 2 'Player2': ref FF01799E VALID [Actor enabled 3D=loaded cell=000125B6 (player's cell) base=FF01799F pos=(-778.7, 1651.5, -161.4) dist=5.6]
[avatar] despawned ref FF01799E 2 s later: no form (gone)
```

| Success criterion | How it is checked | Result |
|---|---|---|
| 1. Valid engine object | `TESForm::LookupByID` finds it; formType `0x4B` (ACHR) (RE §4) | ✅ VERIFIED-RT |
| 2. Valid reference and handle | Created by the engine's own `PlaceActorAtMe` → `TESDataHandler::CreateReference` (RE §2–3). Papyrus can bind it as a script object, which requires a live handle. Raw handle value not inspected. | ✅ (handle value UNKNOWN) |
| 3. Registered in world/cell | `parentCell` (+0xB0) == player's cell | ✅ VERIFIED-RT |
| 4. 3D loaded | Get3D (vfunc 0xAC, the same call as `Is3DLoaded`) non-null; `NONE` at spawn, loaded at +1 s | ✅ VERIFIED-RT |
| 5. Visible | User confirmed visually that the proxies render opaque in both windows (with AI left on) | ✅ VERIFIED-RT |
| 6. Receives transform updates | Proxy position follows the remote pose (Papyrus `SetPosition`/`SetAngle`, 10 Hz) | ✅ VERIFIED-RT (it slides; no walk animation) |
| 7. Safe cleanup | `Disable()` + `Delete()`; 2 s later `LookupByID` returns null; no crash | ✅ VERIFIED-RT |

## Update 2026-11-02 (afternoon): AI, animation, appearance

- **AI cannot be disabled on a live proxy.** Freezing at spawn leaves it semi-transparent. Freezing 3 s after the 3D loads keeps it opaque, but the model then no longer follows position updates (RE §5). The proxy keeps AI on, so neutralizing its behaviour (wandering, combat, dialogue) needs another mechanism.
- **Animation: not solved yet.** Graph variables `Speed`/`Direction` are calibrated (RE §8) and written from network velocity (`AvatarAnimate=1`). The writes stick, but the proxy still slides in a standing pose, so NPC locomotion is selected by the movement system. Next: RE how the engine moves NPCs with animation (decompile the movement path in IDA).
- **Appearance:** cloning a nearby NPC (or a random template) is rejected. The proxy must look exactly like the remote player. The engine's own mechanism is `Actor.CopyAppearance`, which copies a fixed set of TESNPC fields onto the target's **base** (RE §9). Plan:
  1. Default base is now the player character's own base `00000007` (`AvatarBase=player`) instead of a random NPC. On a test rig where both copies load the same save, this already shows the right character.
  2. Give each remote player a **dedicated proxy NPC record**. Either ship a small plugin file with a `CoSync_RemotePlayer` NPC_ record, or clone a base at runtime and spawn through `TESDataHandler::CreateReference` directly (PlaceActorAtMe rejects runtime bases, RE §2).
  3. Serialize the fields `TESNPC::CopyAppearance` copies (RE §9, after `0xCD56E0` is decompiled), plus the worn equipment, and send them reliably on join and on change. Apply them to the dedicated record, then refresh the 3D the way `0x17A96D0` does.

## Why earlier attempts failed (determined)

| Symptom (date) | Root cause | Evidence |
|---|---|---|
| `PlaceAtMe(Form, …)` returned None, "Type mismatch for argument 1" (10-27) | The argument was rejected by Papyrus type checking. `PlaceActorAtMe(actorbase, …)` is the actor-specific native. | Papyrus.0.log; fixed by switching natives |
| `PlaceActorAtMe` returned None for template bases `FF013A02`, `FF0139C6` (10-27) | **By design**: the native rejects any base whose FormID is in `FF000000–FFFFFFFE`, with the message *"cannot be placed with PlaceActorAtMe, as it's a runtime-created form"*. Loaded NPCs built from templates or leveled lists carry runtime bases. | RE §2 (`0x207A2C9..0x207A2F1`). Fixed by resolving the data-file base via Papyrus `GetBaseObject` (commit d507141). |
| Game exited silently at the first spawn (10-27 22:20, 22:24) | The plugin walked `TESObjectCELL::references` and read `Actor::boolBits` with layouts not verified for 1.16.244 | Commit 560d863; four dumps not yet analysed (RE §8) |
| Clone of the player base `00000007` showed only a faint floating head (10-27) | The player's base is not a complete NPC appearance: face and outfit live on the PlayerCharacter instance. The faintness matches the transparency below. | In-game observation; RE §5 |
| **Proxies semi-transparent** (11-02) | `EnableAI(false, true)` called ~10 ms after spawn, before 3D had even loaded. A/B test: without that call the proxies are opaque. | RE §5; user confirmation. Fixed: `AvatarFreezeAI` now defaults to 0. |

## Native actor creation pathways found

1. **`ObjectReference.PlaceActorAtMe`** (Papyrus native `0x207A220`): Allocate → `Actor::Actor` → set base and level → `SetParentCell` → transform → `TESDataHandler::CreateReference`. The engine fully initializes and registers the reference, then loads 3D asynchronously. *Used now.*
2. **`TESDataHandler::CreateReference`** (`0x5A0520`, ID 46570) called directly: the same engine function, synchronous and without the VM, but it needs the new-reference descriptor fully mapped (RE §3 lists what is still UNKNOWN). *Future optimization.*
3. **PlayerCharacter factory** (`0x1891FB0`): constructs a PlayerCharacter and overwrites the global player singleton. *Not usable for proxies* (RE §6).

## Proxy strategy comparison

| Option | Engine support | Risks | Verdict |
|---|---|---|---|
| Second `PlayerCharacter` | **None.** The only constructor path writes the singleton global (RE §6), and CreateReference never builds one. | Engine code assumes one player: camera, input, HUD, quests and saves would treat it as *the* player; overwriting the global corrupts the session. | ❌ Rejected (evidence-based) |
| Custom class derived from `Actor` (our own vtable) | None. The engine constructs Actors in exactly two ctor overloads. | We would have to replicate the vtables (25 for Actor) and the save/load, handle and 3D lifecycles; every patch can break it. | ❌ Rejected |
| **NPC-based proxy via the engine's spawn path** | **Full.** It is the same path the game uses for scripted spawns. | Needs a placeable data-file base. The NPC has AI, combat, dialogue and a faction, which must be neutralized. It is saved into the host's save unless cleaned up. | ✅ **Selected** |
| Custom non-actor reference (e.g. a static mesh / furniture marker) | Supported for any placeable base | No skeleton, animation or equipment, so it is a dead end for player representation | ❌ Only as a fallback marker |
| Reuse an existing NPC already in the world | Supported | Hijacks quest/game NPCs | ❌ |

## Recommended implementation (current and next steps)

Current (implemented in `src/CoSyncSF.Plugin/src/Avatars.cpp`, `GameBridge.cpp`):
1. Wait until the Papyrus VM has been running for 2 s (it is frozen during loads, and calls made then are dropped).
2. Choose a template: `Game.FindRandomActor` near the player → `GetBaseObject` → accept only an `NPC_` whose FormID is not `FF…` (the native rejects those).
3. `player.PlaceActorAtMe(base, 0, None, false, false, true, None, true)`.
4. Do **not** freeze AI (it makes the proxy transparent).
5. Validate with `avatars::Probe` (ACHR, enabled, not deleted, Get3D non-null, same cell) at 1/2/4/8 s, then every 15 s, logging `VALID`/`INCOMPLETE`.
6. Move with `SetPosition`/`SetAngle` at 10 Hz from the interpolated network pose.
7. On leave or cell change: `Disable`, `Delete`, and re-probe 2 s later to confirm it is gone.

Known gaps (next, in this order):
1. **Behaviour control without `EnableAI(false)`.** With AI on, the NPC can walk away between position updates, start dialogue, or join combat. Candidates to research:
   - delay the freeze until the fade-in has finished;
   - an AI package that does nothing, or a "do nothing" faction/actor value;
   - `SetRestrained` (a 1-parameter native registered in the exe; the owning class was not resolved by `natives.py`, and the engine effect is unverified);
   - per-frame `SetPosition` through the C++ `Actor::SetPosition` vfunc (CommonLibSF slot 0x137, unverified) instead of Papyrus.
2. **Appearance.** Copy the remote player's real appearance: sex, race, face morphs, outfit. The player base is incomplete (see above), so this needs a dedicated NPC record (ESM) or runtime face data transfer. Research item.
3. **Animation.** Drive locomotion from the network speed/direction (animation graph variables) instead of sliding.
4. **Persistence hygiene.** Ensure proxies are never written into saves: they are `FF…` refs with `abDeleteWhenAble`, but this needs a save/load test.
5. **Direct `CreateReference`** call, once the descriptor is fully verified (synchronous spawn, no VM latency).

## How to reproduce

```powershell
./scripts/build.ps1
./scripts/deploy-plugin.ps1
./scripts/launch-instances.ps1 -Avatars                  # proxies opaque
./scripts/launch-instances.ps1 -Avatars -AvatarFreezeAI  # A/B: reproduces the transparency
./scripts/launch-instances.ps1 -Avatars -AvatarScale 3   # debug: oversized proxies
```

Read `Documents/My Games/Starfield/SFSE/Logs/CoSyncSF-p1.log` and `-p2.log` for the `[avatar]` lines.
