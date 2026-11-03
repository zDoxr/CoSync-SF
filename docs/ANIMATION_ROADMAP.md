# CoSync-SF — Animation Roadmap

See [ANIMATION_REVERSE_ENGINEERING.md](ANIMATION_REVERSE_ENGINEERING.md) (RE A.n), [ANIMATION_SYNC_ARCHITECTURE.md](ANIMATION_SYNC_ARCHITECTURE.md) and [ANIMATION_TEST_RESULTS.md](ANIMATION_TEST_RESULTS.md).

## Completed

- **Root cause of sliding identified (static evidence).**
  - Locomotion variables are engine-owned: per-frame cache (RE A.3) and process bindings (RE A.4).
  - Movement goes through the actor's `MovementControllerNPC` (RE A.5).
  - `SetPosition` bypasses it.
- **Player vs NPC pathway mapped.**
  - The player forces a motion-driven controller fed by `IMovementPlayerControls` / `IMovementPlayerControlsFilter` (RE A.5).
  - The player graph gets `IsPlayer`/`iIsPlayer` and seven extra bindings (RE A.2).
- The proxy's graph is confirmed live at runtime: the engine wrote its `Speed` while pathing (RE A.8).
- Player animation-event capture works: jump/land/footstep/flavor-idle events are captured in order.
- Native animation probe (`AnimProbe`): engine getters, controller mode/config, transition-based logging, and a getter self-check (`AvatarAnimDiag=1/2`).
- Experiments wired: `AvatarMoveMode=motion` (engine controller re-init as motion-driven) and path-result logging.
- RE tooling:
  - `tools/re/ida/decompile_list.py`: Hex-Rays on chosen functions in ~35 s without full analysis.
  - `tools/re/animvars.py`: string-global names, refs, pseudocode annotation.
  - Full IDA database build restarted (`local/ida/Starfield-1.16.244.i64`).

## Blockers

1. **No in-game run of the new build yet.** All animation rows remain unverified (TEST_RESULTS).
2. Unknown per-frame writer of the locomotion cache (RE A.9 #3). This decides whether L2 can work at all.
3. Unknown `IMovementPlayerControls` layout (RE A.9 #4). Needed for L1, the highest-fidelity driver.

## Next steps (in order; each gated on in-game verification)

1. Run the protocol in TEST_RESULTS with `-AvatarMoveMode motion -AvatarAnimDiagLevel 2`. Record the player vs proxy controller `mode`/`cfg`/`procE7` and the self-check.
2. When the full IDA database finishes:
   - xref the cache users (`GetGraphVariableCacheSize`, slot `0x0E`) to find the `Speed`/`Direction` writer and its inputs;
   - decompile `MovementHandlerAgentPlayerControls` (`0x68DFE0` factory) to recover the `IMovementPlayerControls` vfuncs it calls.
3. **First milestone, stand/walk/run without sliding.** Implement L1:
   - motion-driven proxy controller plus a CoSync `IMovementPlayerControls` that returns the remote move intent;
   - correct drift toward the interpolated pose;
   - send `moveIntent` + `gait` (architecture §4).
   Fall back to fixing L3 if L1 is infeasible.
4. AnimState/AnimEvent messages (seq, keyframe, hysteresis), reusing the existing reliable channel.
5. Jump/fall/land: `iSyncJumpState` (bound, RE A.4) plus allow-listed `JumpUp`/`JumpDown` replay.
6. Weapons: draw/holster (`iSyncWeaponDrawState`), sighted (`iIsSighted`), fire/reload events. Needs equipment sync first.
7. Sneak, ladders, furniture/interaction (`iLadderClimbState`, `DisableAnimationDriven` users, RE A.7).
8. Debug overlay row per proxy from `animprobe::DescribeState` (UI framework; no UI redesign).
