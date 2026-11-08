# CoSync-SF — NPC Synchronization Architecture

Goal: every NPC the host has loaded looks and behaves the same on every client. The host's simulation is authoritative.

## Building blocks already proven (player proxies)

| Need | Proven mechanism | Reference |
|---|---|---|
| Move an actor with real locomotion, no sliding | Movement state 1 plus a CoSync `IMovementPlayerControls` replay object with closed-loop movement input | ANIMATION_REVERSE_ENGINEERING A.10, A.6 |
| Discrete actions (attacks, block, sneak, fire, jump) | Capture graph input events (holder slot 1), replay with `NotifyAnimationGraph`; engine jump with the scoped vfunc 363 hook | A.11, A.12 |
| Weapon state | `AddItem`/`EquipItem`/`DrawWeapon` plus `SetAlert` | test results |
| Network | Tagged transform sections (controls, events, weapon), sequence numbers, redundancy | Protocol 1.3 |

**Key constraint:** `EnableAI(false)` stops the engine from updating an actor's 3D (RE §5). Client copies of NPCs therefore cannot simply be frozen. They are taken over the same way player proxies are, through the movement pipeline.

## Design (protocol 1.6)

What is built and how far it has been tested: [NPC_SYNC_STATUS.md](NPC_SYNC_STATUS.md). The in-game side has **not** been run yet.

### Authority

The server names one **NPC authority**: the lowest connected game client. It stays until it disconnects, then the role moves to the next one, and everyone is told. Tool clients never hold it. Only the authority's states and deaths are relayed. A late message from a former authority is dropped without penalty, since it can be legitimately in flight during a handover.

The authority's game is the real simulation: its AI decides, its engine applies damage, its NPCs die. Clients reproduce the result. The dedicated server never simulates Starfield, so it only routes and enforces.

### Messages

| Message | Direction | Delivery | Content |
|---|---|---|---|
| `NpcAuthority` | S to C | reliable | who owns NPCs. Sent on join and on change |
| `NpcStates` | authority to others | unreliable, latest wins | sequence, location, up to 40 NPCs per message, nearest first: FormID, base, position, yaw, health %, flags. A snapshot spans several messages, up to 512 NPCs |
| `NpcDeath` | authority to others | reliable | FormID. Remembered by the server and replayed to late joiners |
| `NpcDamage` | client to authority | reliable | FormID, health points |

Each message is under 1200 bytes. At 60 NPCs and 10 Hz that is about 16 KB/s per client. Velocity is derived by the receiver. States carry absolute values, so a lost packet repairs itself on the next one, and a late joiner converges within one snapshot. No separate state-restore step is needed for what they carry.

Data-file references match by FormID. Runtime `FF…` references match only if the receiver's reference of that ID has the same base, which holds between copies of one save. Anything else is counted as unmatched and skipped. It is not spawned yet.

### Client takeover

The AI bit gates the entire actor update, 3D included (RE 12.2), so a client's NPC keeps updating and only its movement is replaced:

1. An NPC is taken over only if it has 3D, its controller is in the plain AI state (state 2, motion-driven), it is not in dialogue with this client's player, and a replay slot is free (256).
2. `AttachReplay`, then `EnterPlayerControlsState`, the same order as proxies.
3. Each frame: steer toward the authority's extrapolated position and turn toward its yaw. Snap only after 0.4 s over 3 units.
4. **Release** (`LeavePlayerControlsState`) on death, dialogue, a stale authority (5 s), this client becoming authority, or session end. A taken-over NPC's controller is **held**: its state-changing vfuncs refuse, because in game real AI kept pulling NPCs out of state 1 (status doc, run log). If it leaves state 1 anyway, the NPC is released and retried after 2 s. The cost is that a held NPC also cannot enter furniture or scene states on the client. Those are lost until the authority's actions are replayed.

The local AI keeps running, so local package and combat decisions still happen. Their effect on movement is overridden, but anything else they do is not. This is deliberate for now: an NPC must still be able to shoot the local player, and that hit has to be simulated on the machine of the player who is hit. Whether this holds up is the main open question.

### Damage and death

- Each client compares its NPC's health with the last known value. A drop that the authority did not report is this client's hit: it is forwarded once as `NpcDamage`, and the authority applies it through Papyrus `DamageValue`, so its engine handles bleedout and death.
- Authoritative health then overwrites the client's value, but never down to zero. Only `NpcDeath` kills a client's copy, so one NPC cannot die twice. Deaths are applied with `KillSilent`, and only to loaded NPCs.
- A new authority starts from the deaths it already knows, so it does not re-announce them.

## Risks

- Local AI fighting the takeover on real NPCs: untested, and the first thing to measure (status doc, tests 2 to 4).
- Local quest scripts: deaths and dialogue run script events on every machine. Not investigated.
- Hooking `NotifyAnimationGraph` for all actors (the shared function `0xF09F60`) runs on every graph event of every actor. The hook must filter cheaply (synced-set lookup) and never allocate on non-synced actors.
- Bandwidth: 50 NPCs × 10 Hz × ~40 bytes ≈ 20 KB/s per client before compression. That is acceptable on LAN; prioritization is needed online.
- Suppressing AI at the source, if it proves necessary: see RE 12.2 for the candidate hook.
