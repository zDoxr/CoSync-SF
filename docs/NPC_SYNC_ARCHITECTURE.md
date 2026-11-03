# CoSync-SF — NPC Synchronization (plan)

Goal: every NPC the host has loaded looks and behaves the same on every client. The host's simulation is authoritative.

## Building blocks already proven (player proxies)

| Need | Proven mechanism | Reference |
|---|---|---|
| Move an actor with real locomotion, no sliding | Movement state 1 plus a CoSync `IMovementPlayerControls` replay object with closed-loop movement input | ANIMATION_REVERSE_ENGINEERING A.10, A.6 |
| Discrete actions (attacks, block, sneak, fire, jump) | Capture graph input events (holder slot 1), replay with `NotifyAnimationGraph`; engine jump with the scoped vfunc 363 hook | A.11, A.12 |
| Weapon state | `AddItem`/`EquipItem`/`DrawWeapon` plus `SetAlert` | test results |
| Network | Tagged transform sections (controls, events, weapon), sequence numbers, redundancy | Protocol 1.3 |

**Key constraint:** `EnableAI(false)` stops the engine from updating an actor's 3D (RE §5). Client copies of NPCs therefore cannot simply be frozen. They are taken over the same way player proxies are, through the movement pipeline.

## Plan

1. **Enumerate (host). ✅ VERIFIED-RT (2026-11-03).**
   - CommonLibSF's `ProcessLists` layout is **wrong for 1.16.244**: `+0x20` reads float bit patterns and `+0x58` reads an empty array.
   - The engine walks its actor list in `0x1A9A1A0` (called by `Game.FindRandomActor`, native `0x20095D0`): u32 count at **`+0x60`**, u32 handle array at **`+0x68`**, resolved through the handle manager (CommonLibSF `GetSmartPointer`, ID 35638).
   - In game: 62 NPCs around New Atlantis, **all 62 handles resolved**, distances 15–265 units. All 62 references have data-file FormIDs (match by FormID across machines); many of their bases are runtime `FF…` leveled templates (irrelevant for matching by reference).
   - Code: `src/CoSyncSF.Plugin/src/NpcSync.cpp`.
2. **Identity.**
   - References from data files (FormID not `FF…`) are the same object on every machine with the same load order and save, so they match by FormID.
   - Runtime references (`FF…`, spawned by scripts or leveled encounters) differ per machine. The host must announce them, and clients spawn a stand-in with the same base. This is a later stage.
3. **Wire.** A new `NpcStates` message, host to clients, unreliable, about 10 Hz.
   - Batched per cell: FormID, position, yaw, velocity, weapon/drawn, look pitch, recent graph input events (sequence-numbered, as for players).
   - Nearer NPCs are sent first, within a byte budget.
4. **Client takeover.** For each synced NPC present on the client:
   - movement state 1 plus a replay object (as for player proxies), closed loop toward the host position;
   - event replay; weapon state.
   - The client NPC's AI decisions are overridden by the movement input and the event stream. Combat/aggro side effects (dialogue, packages) must be suppressed. This is open research: candidates are packages, factions, `SetRestrained`, `StopCombat`.
5. **Life cycle:** spawn/despawn, cell changes, death and ragdoll, dismemberment, loot. Later stages.
6. **Combat authority:** damage and health from the host only. Clients send hit requests. Later stage.

## Risks

- Hooking `NotifyAnimationGraph` for all actors (the shared function `0xF09F60`) runs on every graph event of every actor. The hook must filter cheaply (synced-set lookup) and never allocate on non-synced actors.
- Bandwidth: 50 NPCs × 10 Hz × ~40 bytes ≈ 20 KB/s per client before compression. That is acceptable on LAN; prioritization is needed online.
- AI fighting the takeover on clients: watch the controller state as for proxies (state 1 re-entry), measure, then suppress at the source.
