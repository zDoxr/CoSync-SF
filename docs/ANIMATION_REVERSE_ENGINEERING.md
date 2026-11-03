# CoSync-SF — Animation & Movement Reverse Engineering

Starfield **1.16.244.0** (`Starfield.exe` SHA-256 `7e9adb14…0f9d28`, see [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)). All addresses are **RVAs** (VA = `0x140000000` + RVA). Address Library IDs are given where code uses them. Confidence labels are the same as in REVERSE_ENGINEERING.md: **VERIFIED** (disassembly/decompilation, cross-checked), **VERIFIED-RT** (also seen at runtime), **INFERRED**, **UNKNOWN**.

This document answers one question: why a remote-player proxy slides instead of walking, and what the engine actually does to animate a moving character.

## 0. Summary

1. **A proxy's animation graph is alive.** While the engine pathed the proxy (`PathToReference`), it wrote the proxy's `Speed` variable itself (0.31 → 1.10 → 0.85), with no plugin writes (log 2026-11-02 23:12:42–23:12:58, §A.8). Graph, skeleton and update are therefore not the problem.
2. **Locomotion variables are engine-owned.** `Speed`, `Direction`, `TurnDelta` and others are in a per-actor **variable cache** that the engine refills every frame (§A.3). Stance, sight, aim, jump and desired heading/location are **bound to memory in the actor's AI process** (§A.4). A Papyrus `SetAnimationVariable*` on any of them is overwritten, which explains the earlier result "writes stick in readback but nothing animates" (RE §8).
3. **What moves the body is the movement controller** (`Actor+0x240`, `MovementControllerNPC`, §A.5). It is an agent pipeline (planners, handlers, path followers and arbiters, §A.6) with two modes:
   - **motion-driven**: code moves the body, and the graph follows the motion. The engine forces this mode on the **player** and feeds it from `IMovementPlayerControls`/`IMovementPlayerControlsFilter`.
   - **animation-driven**: root motion from the selected clip moves the body.
   `SetPosition` bypasses the controller entirely (a warp), so no locomotion is ever requested. That is the root cause of sliding.
4. **Player vs NPC.** The same graph-variable set-up runs for both. The player's override then sets `IsPlayer=1`/`IsNPC=0`/`iIsPlayer=1` and binds seven extra variables to PlayerCharacter memory (§A.2). An NPC proxy therefore runs the graph with the NPC flags and without those player bindings. A second `PlayerCharacter` is still not constructible (RE §6).

## A.1 `IAnimationGraphManagerHolder` (TESObjectREFR/Actor `+0x60`)

| Slot | Actor | PlayerCharacter | Role (evidence) | Confidence |
|---|---|---|---|---|
| `0x01` | | | NotifyAnimationGraph (used by `AnimEvents::Notify`; matches `ObjectReference.PlayAnimation` native) | VERIFIED (RE §8 note in AnimEvents.h) |
| `0x0A` | `0x18EF1A0` (ID 100672) | `0x1A3B450` (ID 102705) → calls Actor's first | **Initial graph-variable set-up** when a graph manager is bound (§A.2) | VERIFIED |
| `0x0D` | `0x18EF9B0` (ID 100673) | via `0x1A3A4D0` | **Builds the list of cached variables** (§A.3) | VERIFIED |
| `0x12` | | | `GetGraphVariableImplFloat(name, float&)`: `0x18EECF0` calls `vtbl+0x90` with `&"Speed"` | VERIFIED |
| `0x13`, `0x14` | | | int / bool getters (CommonLibSF numbering) | INFERRED; runtime self-check in `AnimProbe::SelfCheckPlayer` |

The vtables were found by RTTI and slot scans: Actor primary `0x4CB9248` (ID 451614, 417 slots), PlayerCharacter primary `0x4CBC448` (ID 452447, 421 slots). CommonLibSF labels slots `0x0A`/`0x0D` as `Unk_0A`/`Unk_0D`, so these roles are new.

**Graph-manager vfuncs** used by slot `0x0A` (`*graphManager` vtable). VERIFIED from the call pattern and argument types:

| Offset | Use |
|---|---|
| `+0xF8` | set vector (`AimAtLocation`, `fBodyMorphOffset`) |
| `+0x100` | set int (`iIsPlayer`, `iSyncIdleLocomotion`) |
| `+0x108` | set float (`fScale`, `fActorScale`, `fHealthPercent`) |
| `+0x110` | set bool (`IsPlayer`, `IsNPC`, `IsFirstPerson`) |
| `+0x140` | **bind** vector to memory (`DesiredLocation`) |
| `+0x148` | **bind** int to memory (`iSyncJumpState`, `iSyncGunActions`) |
| `+0x150` | **bind** float to memory (`fDesiredHeading`, `DirectAt*`) |
| `+0x158` | **bind** bool to memory (`bAimActive`) |

"Bind" means the third argument is an address inside an engine object, not a value (for example `process->middleHigh + 0x56C`). The graph then reads that memory directly.

## A.2 Graph-variable initialization: NPC vs player

**Actor (`0x18EF1A0`)**, set on every NPC including proxies. VERIFIED:
`fScale`, `IsPlayer=false`, `IsNPC=true`, `iIsPlayer=0`, `IsFirstPerson=false`, `bHumanoidFootIKEnable` (global), `bActorMobilityNotFullyCrippled`, `bIsFemale`, `iSyncIdleLocomotion=1`, `AimAtLocation`/`LookAtLocation`, `bLookAtLocationSet=0`, `iSyncSprintState=0`, `bIsPathing=false`, `ReloadSpeedMult`, `iIsPoseB=0`, `iSyncPartialBody=0`, `fActorScale`/`fActorScaleInverse`, `fHealthPercent`, then **`0x1A11C00(process)`** (§A.4), `fGravityScale`, `fBodyMorphOffset`, `iSyncTurnState`, `iSyncForwardState`, `iSyncStrafeState`, `iSyncWeaponDrawState=1`, `FemaleOffset`. It ends with `0xC94060(54, actor, graph)`, purpose UNKNOWN.

**PlayerCharacter (`0x1A3B450`)**: calls the Actor version, then. VERIFIED:
- `IsPlayer=true`, `IsNPC=false`, `iIsPlayer=1`, `bIsSliding=0`, `IsFirstPerson=<arg 3>`.
- Binds to PlayerCharacter memory (holder `+0x1074..+0x108C`): `iSyncGunActions`, `SyncJumpBlend`, `iSyncSighted`, `bDontAllowPowerAttack`, `bDontAllowBlock`, `bAlignWithAimAssistTarget`, `iSwitchingPerspectives`.

Consequences:
- A proxy runs the humanoid graph in NPC configuration. Whether the graph's player branches (keyed on `IsPlayer`/`iIsPlayer`) select player-specific clips is UNKNOWN until the behavior files are read. The pathing test proved that NPC locomotion does play on the proxy graph (§A.8).
- The seven player-bound variables do not exist as live bindings on a proxy. Values written to them with `SetAnimationVariable*` persist, because nothing rebinds them, but they are not driven.

## A.3 Engine-cached locomotion variables (written every frame)

Slot `0x0D` builds the actor's cached-variable list. Actor (`0x18EF9B0`), in order: `Direction`, `VerticalDirection`, `Speed`, `Pitch`, `Roll`, `TurnDelta`, `PitchDelta`, `weaponSpeedMult`, `WeaponAttackAnimationDuration`, `leftWeaponSpeedMult`, `iWantBlock`, `TimeDelta`, `bCCSupport`, `fCCNormDotUp`, `bCCOnStairs`, `TurnDeltaSmoothed`, `SpeedSmoothed`, `BoneWeightsAction`. The player adds (`0x1A3A4D0`): `PitchDeltaSmoothed`, `DirectionSmoothed`, `FirstPersonSpeedSmoothed`, `fControllerXSum/YSum/XSmoothed/YSmoothed/XRaw/YRaw`, `fControllerMoveXRaw/YRaw/ZRaw`. VERIFIED (decompiled).

- `DirectionSmoothed` and the `fController*` variables are player-only. This matches RE §8 ("`DirectionSmoothed` on one of the two graphs only").
- The `fController*` group shows that the player's graph is fed raw stick input. A proxy graph has no equivalent.
- **UNKNOWN:** the per-frame writer that fills this cache (index-based, no name references). It is the key remaining target. The values it writes for the player match measured motion (`Speed` 5.0 at a measured 5.2 u/s, RE §8).

`0x18EECF0` (Actor vfunc 224, ID 100660) reads graph `Speed` back, multiplies by actor scale and stores it at `process->middleHigh + 0x49C`: animation-driven speed feedback into movement. VERIFIED.

## A.4 Variables bound to the AI process (`0x1A11C00`, ID 102486)

Called from Actor's slot `0x0A` with `Actor+0x228` (currentProcess). It binds graph variables to `process+0x08` (middleHigh) memory. VERIFIED:

| Variable | Binding | Offset from `*(process+0x08)` |
|---|---|---|
| `iSyncActorStance` | int | `+0x510` |
| `iSyncRelaxedReadyAlert` | int | `+0x514` |
| `iIsSighted` | int | `+0x518` |
| `iIsBlindFiring` | int | `+0x51C` |
| `iIsGunDown` | int | `+0x520` |
| `iSyncRightLeft` | int | `+0x524` |
| `iPartialCover` | int | `+0x528` |
| `DirectAtHeadingVar` / `DirectAtPitchVar` | float | `+0x52C` / `+0x530` |
| `DirectAtHeadingMin/Max`, `DirectAtPitchMin/Max` | float | `+0x534` / `+0x538` / `+0x53C` / `+0x540` |
| `DesiredLocation` | vector | `+0x550` |
| `fDesiredHeading` | float | `+0x560` |
| `iSightedRequested` | int | `+0x568` |
| `iSyncJumpState` | int | `+0x56C` |
| `bAimActive` | bool | `+0x587` |

These bindings exist on proxies (NPC path). Writing these process fields, rather than calling `SetAnimationVariable*`, is how the engine itself drives aim, stance and jump. **Not yet written by CoSync**: each field's semantics, and whether the AI overwrites it every frame, must be checked first (UNKNOWN).

## A.5 Movement controller: creation and motion- vs animation-driven

- `Actor+0x240` holds a refcounted `MovementControllerNPC` (refcount `+0xA8`). It is built by ctor `0x1AB7390` (ID 103556; 200-byte object, base ctor `MovementControllerAI` `0x2733CA0`) in `0x1922590` (ID 100901) and `0x1922740` (ID 100903). VERIFIED. This matches CommonLibSF `Actor::movementController @0x240` and `currentProcess @0x228`.
- `MovementControllerAI` layout. VERIFIED from ctor and users:
  - `+0x48`: interface registry keyed by interface-name strings. Register `0x2732FD0`, unregister `0x27330A0`, lookup `0x688B60`.
  - `+0x50`: `ControllerInternalState`, the implementation registered as `IMovementMotionDrivenControl`.
  - `+0x58`: form ID.
  - `+0x5C`: mode byte.
  - `+0x70` / `+0x71`: byte / gate written by `0x2738D90`.
  - `MovementControllerNPC` adds `+0xB8`, the owning actor (`a1+184` in vfuncs).
- **Mode switch.** VERIFIED:
  - `0x2739530` (ID 135322) registers `IMovementMotionDrivenControl`; if vfunc 16 succeeds, `+0x5C = 2`.
  - `0x2739590` (ID 135323) unregisters it; if vfunc 17 succeeds, `+0x5C = 1`.
  - So **2 = motion-driven** and **1 = animation-driven**. INFERRED from the interface name; to be confirmed at runtime with `AnimProbe`.
- **NPC selection.** `0x1922590` uses animation-driven if the actor has no process or `process+0xE7 != 0`, otherwise motion-driven. `0x1922740(actor, bool motionDriven)` is called from `0xB4A9C0` with `process+0xE7 == 0`. VERIFIED. What the `+0xE7` byte means is UNKNOWN.
- **Player selection** (`0x1A540E0`, ID 102794, PlayerCharacter override): base set-up, then **forces `0x2739530` (motion-driven)**, `0x2734D10`, and registers `IMovementPlayerControlsFilter` → `PlayerCharacter+0x6A0`, the subobject with COL offset `0x6A0` (RE §6). VERIFIED.
- `MovementControllerNPC` vfunc 16 `0x1AB7B70` (ID 103564) applies configuration `0x61EFDB0` (or `0x61EFDB8` when an actor-flag check passes) and then registers **`IMovementPlayerControls`** pointing at global `0x61E23E0` + `0x20` (ID 938464 for the global; object type UNKNOWN, believed to be the `PlayerControls` input singleton). Vfunc 17 `0x1AB7C80` (ID 103565) applies `0x61EFDC0`/`0x61EFD88` and unregisters `IMovementPlayerControls`. VERIFIED.
- **CommonLibSF naming conflict.** CommonLibSF names ID 135315 (`0x2738EF0`) `SetMotionDriven` and ID 135316 (`0x2738F80`) `SetAnimationDriven`. Both only call `0x2738D90(ctrl, 1 or 2)`, which writes `+0x70`. `0x2738EF0` is also called unconditionally after either mode is chosen. Those names are therefore **not** supported by the disassembly. CoSync does not use them.

## A.6 Movement configurations (agent pipelines)

- RTTI lists the movement system's classes. They include `MovementArbiter`, `MovementPlannerArbiter`, `MovementHandlerArbiter`, `MovementPathManagerArbiter`, `MovementPostUpdateArbiter`, the agents `MovementAgentPathFollowerStandard`/`Virtual`/`Flight`, `MovementHandlerAgentPlayerControls`, `…PlayerControlsActionTrigger`, `…SprintActionTrigger`, `…DirectControl`, `…GraphDrivenAnimationDriven`, `…Standard`, `…StairsHelper`, `MovementPlannerAgentDirectControl`, `…Warp`, `…NavmeshBounds`, `MovementTweenerAgent`, and about 40 `IMovement*` interfaces (`IMovementDirectControl`, `IMovementPlannerDirectControl`, `IMovementQueryAnimDeltas`, `IMovementAnimationDrivenFeedback`, `IMovementMotionDrivenControl`, `IMovementSetSprinting`, …). VERIFIED (RTTI names).
- A configuration is a set of 5 arbiters plus a list of agent factories. Builders: for example `0x1ABD060` → global `0x61EFDB0` and `0x1ABD290` → `0x61EFDB8`. The arbiters are `0x2707330`, `0x2709E20`, `0x270B730`, `0x270C430`, `0x270D300`. The motion-driven configs include the PlayerControls agent factories (`0x68E360`, `0x68BF40`, `0x68C4F0`, `0x68E780`) and the direct-control/sprint/warp agents (`0x1AC8AB0`, `0x1AC83A0`, `0x1AC9C20`, `0x1AC96E0`, `0x1ACA4E0`). VERIFIED (call lists). Which factory creates which class: INFERRED from address proximity to the class ctors.
- `MovementControllerNPC` keeps a configuration state at `+0xC0`, initialized to 2. VERIFIED. Its vfuncs apply configurations through `0x2735FB0`:

| State | Config | Applied when |
|---|---|---|
| 2 | `0x61EFD90` | `+0x5C == 2`, default |
| 7 / 8 / 12 | | `+0x5C == 2` with flags `+0xC5` / `+0xC6` / `+0xC7` |
| 10 | `0x61EFDF8` | animation-driven |
| 3 / 11 | | second vfunc, motion / animation |
| 1, 13, 0, 4, 5, 9 | | other vfuncs |

  Naming the states needs their agent lists compared (UNKNOWN).
- `0x18A07D0` tears all configurations down (one per global), confirming 27 configurations `0x61EFD68..0x61EFE38`.

## A.7 Graph-variable name inventory

318 names in the cluster `0x5E89B20..0x5E8A5D8` are static `BSFixedString` globals, built by ctor `0x28CBA80`. `tools/re/animvars.py names` lists all 7,992 such globals in the exe, and `… refs` lists their code references. Selected writers (VERIFIED by reference; roles from decompilation where noted):

| Variables | Code |
|---|---|
| `IsPlayer`, `IsNPC`, `iIsPlayer`, `IsFirstPerson` | Actor `0x18EF1A0`, player `0x1A3B450` |
| `iSyncSprintState` | 11 sites, incl. `0x18EE1B0`, `0x1925690`, `0x19AD760`, `0x1A5DD20/DE0` (player), `0x12BE1C0`, `0x108E380` |
| `iSyncForwardState`/`StrafeState`/`TurnState`/`IdleLocomotion` | `0x18EF1A0`, `0x19AD480`, `0x19C0380`, `0x1A18580`, `0x1A19390` |
| `iSyncWeaponDrawState` | `0x18EF1A0`, `0x1913C50`, `0x1AFE6F0` |
| `iSyncGunDown` | `0x12C3FE0`, `0x17913A0`, `0x198E860`, `0x19C4880`, `0x19D9990` |
| `iIsInSneak` | static init only (graph-internal or index-based) |
| `fWalkSpeed`/`fFastWalkSpeed`/`fJogSpeed`/`fRunSpeed`/`fSprintSpeed` | `0x18ED020` (callers `0x18ECC60`, `0x18ECDF0`, `0x1958550`, `0xC7F080`) |
| `DisableAnimationDriven` | 25 sites (furniture, paired, ship, scene code) |
| `MovementDirection` | `0x19D8490`; `VelocityZ` `0x1953990`; `RootMotion` `0x1AE85D0` |

## A.8 Runtime findings (two-instance rig, 2026-11-02)

| Observation | Source | Meaning |
|---|---|---|
| Teleport mode: proxy follows the pose in a standing pose. Graph writes of `Speed`/`Direction` stick in readback but nothing animates. | RE §8, user observation | Warp + engine-owned variables (§A.3, §A.5) |
| Path mode (`PathToReference` toward a marker): the engine sets the proxy's `Speed` 0.31 / 1.10 / 0.85 / 1.02 while it closes a 7.7-unit gap in about 6 s (≈1.1 u/s). Peak ≈1.1 even at `afNormalizedSpeed = 1.0`. | `CoSyncSF-p1.log` 23:12:42–23:13:04 | The proxy's graph and movement controller are live; AI pathing selects a **walk**. Whether the walk cycle was visible: not recorded. |
| Path mode then stalls at a constant 7.38 gap while path calls rise from 38 to 80 in 10 s | same log, 23:13:09–23:13:19 | `PathToReference` returns immediately (refused or "arrived"). The return value is now logged (`[path] … returned`). |
| Player animation events (sink hook, vtable ID 452451): `FootLeft`, `SoundPlay \| FST_Foley_ArmorType_Run`, `JumpUp`, `JumpDown`, `JumpLandAdditive`, `JumpLandStop \| 0.100`, `JiggleEvaluate`, `IdleFlavorLarge`/`IdleFlavorLargeEnd` | both logs | Discrete actions (jump/land/flavor idles) are visible as graph events on the sender. No move-start/stop events (locomotion is variable-driven). |
| Player graph idle: `Speed=0`, `Direction=0.5`, `TurnDelta` 0 → ±160 while turning in place | logs | `Direction` holds its last value at rest |

## A.9 Open items, in order of value

1. **Runtime check with `AnimProbe`** (`AvatarAnimDiag=1`): the controller mode/config/`procE7` for the player vs a proxy, and the getter self-check.
2. **`AvatarMoveMode=motion`: answered (VERIFIED-RT, 2026-11-02 17:44).**
   - Proxies are already motion-driven (`mode=2`, `procE7=0`), and the re-init is a no-op.
   - The player runs config state **1**, the proxy **2**.
   - Under teleport the proxy's `Speed` stays 0.00 while the sender's reads 5.00 (run) or 7.05 (sprint).
   - The player's `Speed` sits at exactly 5.00/7.05 rather than tracking measured speed, so it appears to be the controller's *desired* speed, not the physical speed. This favours L1 (feed movement intent).
   - The proxy's AI starts a path about 10×/s (`bIsPathing` flicker), fighting the teleports.
3. The **per-frame writer of the variable cache** (§A.3) and what it reads (character-controller velocity vs. controller deltas). The full IDA database (`local/ida/Starfield-1.16.244.i64`, analysis running at the time of writing) is needed for xrefs from `GetGraphVariableCacheSize` (slot `0x0E`) users.
4. **`IMovementPlayerControls`** interface layout: the object behind global `0x61E23E0 + 0x20` and its vtable. If a proxy controller can be given its own implementation, the proxy moves through the player's own pipeline with remote input (see the architecture doc).
5. Semantics of `process+0xE7`, and of the `middleHigh` fields in §A.4 (who writes them per frame).
6. Behavior graph files (`.hkx` in the BA2s): which branches use `IsPlayer`/`iIsPlayer`.

## A.10 Player input path: `IMovementPlayerControls`

| Item | Evidence | Confidence |
|---|---|---|
| The controller's interface registry (`MovementControllerAI+0x48`) is an array of `{BSFixedString key, void* value}` at `+0x10` (count `+0x10`, capacity/inline flag `+0x18`, data `+0x20`) with a lock at `+0xD0`. Lookup `0x688B60` (ID 49356) takes a shared lock and returns the first match. Register `0x2732FD0` (ID 135280) appends without deduplicating. Unregister `0x27330A0` (ID 135281) removes. | decompiled | VERIFIED |
| `MovementHandlerAgentPlayerControls` (ctor/factory `0x68DFE0`, 152 bytes) registers `"Set Player Controls"` → agent `+0x20` and `IMovementQueryDesiredDeltasInterface` → agent `+0x28` on its controller (`0x68C300`). It unregisters them in `0x68C380` and the dtor. **The presence of `"Set Player Controls"` in a registry means the agent is installed.** | decompiled | VERIFIED |
| Its per-frame handler `0x68C740` looks up `IMovementPlayerControlsFilter` (optional) and `IMovementPlayerControls`, then calls the latter's **vfunc 1 `(iface, uint32 id, ControlsData* out)`** on a caller-initialized buffer, and converts the result into desired movement. | decompiled | VERIFIED |
| `id` is an **actor FormID**: the engine's implementation resolves it with `0x5DE3B0`, a FormID → form hash-map lookup, and checks formType `0x4B`. | decompiled | VERIFIED |
| The engine's implementation is `PlayerControls::Manager` (global pointer ID 938464 at `0x61E23E0`; primary vtable ID 433847). Its `+0x20` subobject has vtable ID 433843 (`0x4C43FA0`): slot 1 = fill `0x12C5E60` (ID 82448), slots 2/3 = per-actor list add/remove `0x12C6350`/`0x12C6440`. Controller vfunc 16 (`0x1AB7B70`) registers `Manager+0x20` under `IMovementPlayerControls`. | RTTI + decompiled | VERIFIED |
| `ControlsData` fields (written by `0x12C8D30` ground and `0x12C91B0` other): `+0x00` float (sign-flipped turn value), `+0x08` move-direction angle (atan2 of the stick vector), `+0x10` qword/xmm, `+0x18` turn rate when aligned to the camera, `+0x30` move magnitude 0..1 × walk factor, `+0x34` look value, `+0x40..+0x4F` turn/pitch accumulators (partly from a sub-object vfunc `+0x30`), `+0x54` byte (from the process). Nothing above `+0x57` is written. | decompiled | VERIFIED (offsets); semantics INFERRED |
| The configuration IDs `0x61EFD68..0x61EFE38` are **32-bit** IDs (`mov dword [..], eax` in their builders), not pointers. The builders for `0x61EFDB0`/`0x61EFDB8` (vfunc 16's configs) add 23 and 26 agent factories respectively. The movement-state configs (`0x2735FB0` users) are built another way. | disassembly | VERIFIED |

**Approach this enables (L1 in the architecture doc):**
- **Capture.** Hook the Manager's fill and keep `ControlsData` for FormID `0x14`.
- **Replay.** Register a CoSync `IMovementPlayerControls` on the proxy's controller that serves that data.
- **Effect.** The proxy then moves through the player's own movement pipeline, and the graph's locomotion variables come from the engine.

`AvatarMoveMode=mirror` is the proof test: the proxy replays the *local* player's data, with no network and no teleports.

## Reproduction

```
python tools/re/sfre.py vtbl .?AVMovementControllerNPC@@        # vtables / ctor xrefs
python tools/re/animvars.py names names.json                     # BSFixedString globals
python tools/re/animvars.py refs names.json refs.json            # code using graph-variable names
set COSYNC_DECOMP_IN=list.txt & set COSYNC_DECOMP_OUT=out.c
idat -A -a -c -Ldecomp.log -S"tools/re/ida/decompile_list.py" -oscratch.i64 Starfield.exe   # ~35 s, no full analysis
python tools/re/animvars.py annotate names.json out.c out_named.c
```

## A.11 Action events (graph inputs), observed on the player (2026-11-03)

Hooking `NotifyAnimationGraph` (holder slot 1) on the PlayerCharacter holder vtable (ID 452463) captured what the engine sends *into* the player's graph:
- `JumpStart`/`JumpLand`, `SneakStart`/`SneakStop`, `MeleeAttackStart`, `MeleeAttackFinisherStart`;
- `FireSingleStart`/`FireRelease`, `SightedStart`/`SightedEnd`/`SightedStopRequest`, `blockStart`/`blockStop`;
- `Equip`/`Unequip`, `RelaxedStart`, `GunDownStart`/`GunDownStop`.

Replaying the allow-listed ones on a proxy with the same call plays the actions there. VERIFIED-RT: melee, crouch, block, fire and the jump animation.

## A.12 Jump physics

- `JumpAnimEventHandler` (vtable `0x4CC62E0`) calls `0x18E53E0(actor, 1.14)` (ID 100537; the height is global ID 500287). That function sets the character controller's jump height (`cc+0x344`) and calls its jump vfunc (`+0x210`).
- For NPCs it requires actor vfunc 363 (`0x1983620`): `Actor+0x250` (CombatController) must be non-null with `+0x1E4` clear. The player overrides this check (`0x1A789E0`).
- A proxy has no combat controller, so the jump is refused. A hook that answers true only for the actor CoSync is jumping, during that call, fixes it. VERIFIED-RT: the proxy jumps with height.

## A.13 Look and aim

- The player's look pitch is the graph variable `fCameraPitch` (−0.76..1.18 rad observed). Writing it on an NPC graph sticks but changes nothing visible (the NPC graph does not read it).
- NPC aim uses `DirectAtPitchVar`/`DirectAtHeadingVar`/`bAimActive`, bound to process `+0x530`/`+0x52C`/`+0x587` (A.4). Writing them every frame was **not visibly effective** (in-game 2026-11-03).
- The player's actor yaw (`data.angle.z`) is the aim/camera yaw, not the visible body facing in third person. Copying it made the proxy backpedal where the player turns around. A facing heuristic (aim yaw when drawn, travel direction when holstered) was **not sufficient** either.
- **Open:** send the sender's real body rotation (its 3D root node) and find the NPC aim path that actually moves the upper body.
