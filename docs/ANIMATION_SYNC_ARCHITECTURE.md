# CoSync-SF — Animation Synchronization Architecture (proposal)

Status: **design, partly implemented (diagnostics only).** Engine facts are cited as "RE A.n" (sections of [ANIMATION_REVERSE_ENGINEERING.md](ANIMATION_REVERSE_ENGINEERING.md)). Nothing here claims a working animation. In-game results go in [ANIMATION_TEST_RESULTS.md](ANIMATION_TEST_RESULTS.md).

## 1. Principles

1. **The proxy's own engine animates it.** We replicate *intent and state*, never poses or frames.
2. **Drive the engine through the inputs it owns.** The variables the graph reads are engine-owned: the per-frame cache (RE A.3) and the process bindings (RE A.4). We feed the systems that write them (the movement controller, process fields, graph events), not the graph variables directly.
3. **The network decides *what*, the game thread decides *when*.** Decoding happens off-thread; every engine call runs in `GameBridge::Tick` (SFSE main-thread task).

## 2. Proxy type

| Option | Verdict | Evidence |
|---|---|---|
| Second `PlayerCharacter` | ❌ impossible: its only factory overwrites the player singleton | RE §6 |
| **NPC `Actor` (current proxy)** | ✅ keep | The graph is live and the engine drives its locomotion (RE A.8). The same graph-variable set-up as the player runs (RE A.2). |
| NPC + player-style controller | ⏳ target | The engine already supports a motion-driven controller fed by `IMovementPlayerControls` (RE A.5). Needs the interface layout (RE A.9 #4). |

Known NPC-proxy limitations (RE A.2/A.3):
- `IsPlayer=0` / `IsNPC=1`.
- The seven player-only bound variables are missing (`iSyncGunActions`, `SyncJumpBlend`, `iSyncSighted`, …).
- There are no `fController*` stick variables and no `DirectionSmoothed`.

Locomotion, weapons and jumps all exist on NPC graphs, so these are fidelity gaps, not blockers. Setting the player flags on a proxy graph is a later, measured experiment (graph branches keyed on them are UNKNOWN).

## 3. Locomotion: candidate drivers, in order

| # | Driver | How | Status |
|---|---|---|---|
| L1 | **Player-controls pipeline** | Put the proxy's controller in motion-driven mode (RE A.5) and give it a per-proxy `IMovementPlayerControls` that returns the *remote player's* move vector, sprint, jump and so on, plus a small correction toward the authoritative position. The engine then computes `Speed`/`Direction`/`TurnDelta` exactly as for a local player. | Needs RE A.9 #4. Highest fidelity. |
| L2 | Motion-driven + engine motion | `AvatarMoveMode=motion` re-initializes the controller (engine function `0x1922740`, ID 100903) and keeps teleporting. Tests whether motion-driven mode derives graph speed from warps. | **Implemented as an experiment**, untested |
| L3 | AI path following | `AvatarMoveMode=path`, marker + `PathToReference`. The engine walks the proxy (RE A.8). | Implemented. Walk only (~1.1 u/s), stalls, latency. Fallback at best. |
| L4 | Graph-variable writes | `AvatarAnimate=1` | ❌ Disproved: engine-owned (RE A.3) |

## 4. State model (what is replicated)

Derived from the remote transform stream, not sent (computed on the receiver from the interpolated pose; this already exists in `DrivePath`):
- planar velocity → speed and direction relative to facing;
- yaw rate → turn;
- vertical velocity → falling/rising.

Sent explicitly. **AnimState**: a compact snapshot, replicated on change.

| Field | Bits | Source on the sender (local player) |
|---|---|---|
| `seq` | u16 | per-player counter, incremented on every change |
| `moveIntent` | 2×i8 | the player's move input X/Y (L1 needs it; `fControllerMove*Raw` on the player graph, RE A.3) |
| `gait` | 2 | walk / run / sprint (`iSyncSprintState`, controller sprint flag) |
| `sneak` | 1 | `iIsInSneak` |
| `grounded` / `jumpState` | 1 + 3 | `iSyncJumpState` (bound, RE A.4) |
| `weaponDrawState` | 2 | `iSyncWeaponDrawState` |
| `sighted` | 2 | `iIsSighted` / `iSyncSighted` |
| `stance` / `relaxedReadyAlert` | 3 + 2 | `iSyncActorStance`, `iSyncRelaxedReadyAlert` |
| `gunDown` | 1 | `iSyncGunDown` |
| `equippedWeapon` | FormID | inventory (separate system) |
| `furniture` / `interaction` | FormID + state | later milestone |

Every field above is a real, engine-named variable (RE A.7). Which of them changes for which action on the player will be **measured with `AnimProbe`** before the message is frozen. The probe logs exactly these transitions for the local player.

**AnimEvent**: one-shot actions captured from the sender's graph-event sink (already implemented, `AnimEvents.cpp`): `JumpUp`/`JumpDown`/land, weapon fire, reload, melee and others. Each carries `{seq, timestampMs, tag, payload}`. It is applied on the proxy with `NotifyAnimationGraph` (holder slot 1) *only* for an allow-listed set. Footsteps, sounds and `JiggleEvaluate` are produced locally by the proxy's own graph and must not be replayed.

## 5. Network handling

| Problem | Rule |
|---|---|
| Bandwidth | AnimState on change only, plus a 1 Hz keyframe. Locomotion rides the existing 20 Hz transform stream. |
| Out-of-order | Per-player `seq` (wrapping u16 compare). Older AnimStates are dropped; AnimEvents are deduplicated by seq. |
| Loss | AnimState is a full snapshot, so the next change or keyframe repairs it. AnimEvents go on the reliable ordered channel. |
| Late events | An event older than 500 ms (receiver clock + sender offset from RTT) is dropped, so a stale jump never plays after landing. |
| Oscillation | Gait thresholds with hysteresis (walk ↔ run ± 10 %) and a minimum dwell of 150 ms before any discrete change is applied to the proxy |
| Restart spam | A discrete state is applied only when it differs from what the proxy already shows (read back with `AnimProbe`). The same event is never re-sent within its dwell. |
| Latency | Interpolation (`TransformInterpolator`, ~100 ms behind) stays separate from animation. L1 leads the move intent by the interpolation delay so the gait matches the visible motion. |

## 6. Threading and lifetime

```
network thread: receive → decode → validate → queue (bounded, already exists for transforms)
main thread (GameBridge::Tick, SFSE task):
   drain queue → per-player AnimState/AnimEvent buffers
   for each proxy: LookupByID(refId) → is ACHR, enabled, 3D loaded? → apply → AnimProbe read-back
engine: graph/controller update in its own frame (never called directly by CoSync)
```
- Actors are never cached as pointers across frames; they are looked up by FormID every tick (`AnimProbe::Read`, `avatars::*`).
- Nothing touches a proxy before its 3D has loaded or after `Despawn` (the existing `loaded3DAt` gate).
- The controller re-initialization runs at most once per spawn. Graph events are rate-limited by the rules above. Nothing is forced every frame.
- The animation-event capture hook runs on whichever thread the engine dispatches events from (UNKNOWN). It only copies strings into a mutex-protected bounded queue and calls the original.

## 7. Diagnostics (implemented)

`AvatarAnimDiag` (INI / `launch-instances.ps1 -AvatarAnimDiagLevel n`):
- **1**: `[anim-state]` lines for the player and every proxy: controller (present, mode `+0x5C`, config `+0xC0`, `+0x70`), `process+0xE7`, and all int/bool graph variables. The full state is logged once, then **only changes**. A getter self-check runs on the player. `[anim-event]` lines carry the player's graph events.
- **2**: adds `[anim-float]` (`Speed`, `Direction`, `TurnDelta`, …), at most 2 Hz and only when changed.
- Path mode logs every change in the `PathToReference` result.

Failure triage with these lines:

| Symptom in log | Category |
|---|---|
| `graph=NONE` on the proxy | missing graph initialization / wrong skeleton |
| `ctrl=NONE` | movement controller not created (proxy construction) |
| getter self-check FAILED | probe slot assumption wrong; distrust int/bool values |
| proxy mode/cfg differs from the player's | movement pathway differs (expected; this is what L1/L2 change) |
| floats stay 0 while the pose moves | engine not producing locomotion (warp) |
| player transitions present, proxy none | network/state application missing |

A debug overlay (UI_FRAMEWORK) can show the same snapshot: `animprobe::DescribeState` is already a one-line summary per actor.
