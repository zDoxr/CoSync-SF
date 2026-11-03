# CoSync-SF — Animation Test Results

Rig: two Starfield 1.16.244 copies on one PC (`scripts/launch-instances.ps1 -Avatars`), both loading the same save. One person moves one copy's player, and the other copy shows the proxy.

Status legend: ✅ pass (seen in game) · ❌ fail (seen in game) · ⚠️ partial · ⏳ not tested · 🚫 not implemented. A result counts only if it was **seen in game** or is backed by a log line.

## Run log

| Date | Build / mode | What was observed | Evidence |
|---|---|---|---|
| 2026-11-02 (earlier) | teleport (`SetPosition` 10 Hz) | Proxy follows the position in a standing pose (slides). Writing `Speed`/`Direction` (`AvatarAnimate=1`) changes nothing visible. | user observation; RE §8 |
| 2026-11-02 17:11 | path (`PathToReference` → marker), `AvatarAnimDiag=1` | Engine sets proxy `Speed` 0.3–1.1 while it closes the gap at ≈1.1 u/s. It then stalls at a 7.38 gap while path calls repeat ~4/s. Visual result not recorded. | `CoSyncSF-p1.log` 23:12:42–23:13:19 |
| 2026-11-02 17:44 | native `AnimProbe`, `AvatarMoveMode=motion`, `AvatarAnimDiag=2` | **No animation control** (user: "0 animation control"). Getter self-check passed. The proxy was **already** motion-driven (`mode=2`) before the experiment, so the re-init was a no-op (`mode 2->2 cfg 2->2`). Player config state **1**, proxy **2**. While the sender ran (player `Speed=5.00`) and sprinted (`7.05`, `iSyncSprintState 0->1`), the proxy's `Speed` stayed **0.00**. The proxy's own AI keeps starting to path: `bIsPathing`/`iSyncIdleLocomotion` flip about 10×/s (153 + 55 toggles), fighting the teleports. Player jump = `iSyncJumpState 0→1→2→0` over ~1.2 s. | `CoSyncSF-p1/p2.log` 23:44:42–23:45:51 |

## Checklist

| Test | Expected result | teleport | path | motion | Notes |
|---|---|---|---|---|---|
| Idle | Natural standing animation | ⚠️ stands; AI idles/wanders | ⚠️ | ⏳ | AI flavor idles are the NPC's, not the player's |
| Walk | Correct walking animation | ❌ slides | ⚠️ engine walks (`Speed` ≈1.1), visual unconfirmed | ❌ slides | |
| Run | Correct running animation | ❌ | ❌ never exceeds walk speed | ❌ proxy Speed 0.00 | |
| Sprint | Correct sprint animation | ❌ | ❌ | ❌ | needs `gait` state (architecture §4) |
| Jump | Correct jump transition | ❌ | ❌ | ❌ | sender events captured (`JumpUp`/`JumpDown`/`JumpLandStop`); replay 🚫 |
| Fall | Correct falling animation | ⏳ | ⏳ | ⏳ | |
| Land | Proper landing transition | ⏳ | ⏳ | ⏳ | |
| Crouch / sneak | Correct crouched movement | 🚫 | 🚫 | 🚫 | |
| Aim | Correct aiming pose | 🚫 | 🚫 | 🚫 | bound process fields (RE A.4) |
| Fire | Correct weapon firing animation | 🚫 | 🚫 | 🚫 | |
| Reload | Correct reload sequence | 🚫 | 🚫 | 🚫 | |
| Melee | Correct melee animation | 🚫 | 🚫 | 🚫 | |
| Draw weapon | Smooth weapon transition | 🚫 | 🚫 | 🚫 | |
| Holster weapon | Smooth return to idle | 🚫 | 🚫 | 🚫 | |
| Direction change | Correct directional blending | ❌ | ⏳ | ⏳ | |
| Stop moving | Returns naturally to idle | ⚠️ (never left idle) | ⏳ | ⏳ | |
| Rapid direction / speed changes | No stutter or oscillation | ⏳ | ❌ stalls | ⏳ | |
| Animation interruption | Recovers, no stuck state | ⏳ | ⏳ | ⏳ | |
| Weapon switch while moving | | 🚫 | 🚫 | 🚫 | |
| Jump while moving | | ⏳ | ⏳ | ⏳ | |
| Crouch while aiming | | 🚫 | 🚫 | 🚫 | |
| Enter / leave combat | | 🚫 | 🚫 | 🚫 | |
| Disconnect during animation | Proxy despawns cleanly | ✅ despawn verified (no animation) | ⏳ | ⏳ | REMOTE_PROXY_RESEARCH criterion 7 |
| Reconnect while other player moves | Proxy respawns and follows | ⏳ | ⏳ | ⏳ | |

## Next run protocol (fills the `motion` column and the first-milestone rows)

```powershell
./scripts/launch-instances.ps1 -Avatars -AvatarMoveMode motion -AvatarAnimDiagLevel 2
```
1. Load the same save in both windows. Wait for `[avatar] … motion-driven controller requested: …` in `CoSyncSF-p1.log`.
2. In window 2: stand 5 s, walk forward 5 s, stop, run 5 s, stop, sprint, strafe left/right, walk backwards, jump in place, jump while running.
3. Watch the proxy in window 1 and note for each step: stands / slides / walks / runs.
4. Copy back both logs. The useful lines are `[anim-state]` (player vs proxy mode/cfg/transitions), `[anim-float]`, `[anim-event]` and the self-check verdict.

Repeat with `-AvatarMoveMode path` to collect the `[path] … returned` values for the stall.

## Controls mode (2026-11-03): proxy driven through the engine's player-movement pipeline

| Test | Result | Notes |
|---|---|---|
| Walk / run | ✅ real locomotion animation (user) | proxy in movement state 1, CoSync `IMovementPlayerControls` |
| Jump | ✅ animation and height (user) | graph replay plus the engine jump (A.12) |
| Crouch, melee, block/guard | ✅ replicated (user) | graph-input event replay (A.11) |
| Fire | ✅ (user) | `FireSingleStart`/`FireRelease` replay |
| Weapon equip/draw | ✅ equips and stays drawn (user) | Papyrus `AddItem`/`EquipItem`/`DrawWeapon` plus `SetAlert` |
| Turn left/right | ⚠️ follows the aim yaw; body facing wrong in third person | A.13 |
| Aim up/down | ❌ | A.13 |
| Stay in sync while moving | ⚠️ closed-loop movement added; not yet confirmed | snap only above 3 units for 0.4 s |
| Slow walk / sprint | ⏳ | sprint is not in the controls struct |
| Event latency | median 64 ms, p90 207 ms (one PC, two copies) | before the immediate-send change |
