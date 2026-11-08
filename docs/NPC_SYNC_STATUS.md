# CoSync-SF — NPC Synchronization Status

The single place to see what NPC sync does today. Design: [NPC_SYNC_ARCHITECTURE.md](NPC_SYNC_ARCHITECTURE.md). Engine evidence: [REVERSE_ENGINEERING.md §12](REVERSE_ENGINEERING.md#12-npc-state-actor-update-and-control-handover).

Status words, used strictly:

| Status | Meaning |
|---|---|
| **VERIFIED WORKING** | Seen working in game on two clients |
| **VERIFIED (tests)** | Proven by automated tests. Says nothing about the game |
| **EXPERIMENTAL** | Code exists and compiles, never run in game |
| **PARTIALLY WORKING** | Works in game with known gaps |
| **UNIMPLEMENTED** | No code. Investigation may or may not have started |
| **BLOCKED** | Cannot proceed until the named thing is resolved |

## Where this stands (2026-11-07)

**Deaths of runtime NPCs, two copies.** A recording showed an NPC killed on the authority still alive on the other copy. Deaths were looked up under the authority's number, while a runtime NPC has another number on each copy, and crowd actors were not searched at all. Both are fixed: copy 1 killed `FF0111CD`, and copy 2 applied it to its own NPC 0.06 s later.

**Walk-ins follow the authority (issue #2), two copies.** Each copy's CrowdManager spawned its own walk-ins, so people walked in on one copy only (the user saw this in a play-test). Now a guest copy does not spawn walk-ins: a hook on the two calls in `0x1275B30` to `0x1276A70` (IDs 81575, 81577) returns at once on a guest. Instead, each outfit list (protocol 1.13) carries the data-file template of every generated NPC, and a guest places an NPC from that template where the authority's walk-in is, pairs it, and removes it when the authority stops reporting it. In 8 minutes: 16 placed, 7 removed again, 0 to 1 unmatched, no freeze. Seated on both copies rose to 82, seated on one side only fell to 13. The placed NPC is from the same template, so it can still look different in detail. A crowd actor with no partner after 10 s is also hidden, as a fallback.

**Correction, same day.** Disabling an actor (`DisableNoWait`) and activating one went through a lookup that refuses actors, so placed walk-ins counted as "removed" were not removed, and hidden walk-ins were not hidden. Fixed in the next commit: every reference lookup in `Avatars.cpp` now accepts actors. Checked again with the fix, 8 minutes on two copies: 20 placed, 9 removed again, 0 unmatched, no freeze. Seated on both copies rose from 68 to 80 over the last minute, seated on one side only stayed between 17 and 21.

**Conversations (issue #16), implemented.** A line said in a conversation one player is in is sent as text (protocol 1.14, `DialogueLine`) to the players near them, and shown on their HUD. Lines come from the game's subtitle event (`ShowSubtitleEvent`, ID 86874), verified in game: it reported every line the guard `0006423E` said. A line counts as part of the conversation when its speaker has the in-dialogue-with-player flag (`Actor+892` bit 8). That check could not be exercised: activating an NPC by script only gets its passing remarks, never a conversation. Unit tested: the server passes lines only to players who may share the place. Not yet spoken aloud on the other copies, which needs the line's form.

**Ambient remarks differ between copies.** The same guard says different lines at different times on each copy. Not handled.

**Guest copy froze once (issue #23).** 90 s into a two-copy run, copy 2's actor update job never finished. Under investigation.

**Presence filter, two copies.** The server now sends NPC states, graph events and outfits only to players who may see them: same cell, or both outdoors within 8192 units (`presence::MayShare`). Deaths still go to everyone. Copy 1 boarded its ship while copy 2 stayed in New Atlantis: in the next minute 1,286 NPC messages were held back, copy 2's NPCs went back to its own AI after 5 s, and its count of received updates stopped at 638.

**Crowd following works in game.** Two copies, 16 minutes, no faults: seated on both copies median 75 (was 27), seated on one side median 15 (was 71). The first minutes were better, 92 and 1. The crowd hook installed, and 85 of 104 crowd actors followed at the start, falling to about 64 as each copy spawned and despawned its own (issue #2). Frame rate with inhibition off is still unmeasured.

Earlier status, 2026-11-06:

**Crowd rework, statically derived, not run.** The last run showed why sync broke down: 98 NPCs seated on the authority, only 27 on both copies, because a crowd actor released to sit then took its own random decisions. CrowdManager cannot be seeded (RE 12.10), so a replica now takes the choice of target from the authority and lets the engine do the walking and sitting, and per-camera inhibition is off during a session. 110 of 110 automated tests pass. Whether it holds in game is unknown until the next run: read the `[crowd] … following now` line and `seated on both` against the numbers above.

Earlier status, 2026-11-04:

**Two in-game runs done, third build not yet run.** Current priority, set by the user: position, orientation, model, clothing and armor, held items, animations, and no pausing in menus. Health and attacks are on hold. The second build holds NPCs reliably. The third adds actions, equipment and the menu patch, and has not been run. 103 of 103 automated tests pass. Dialogue, quests and world objects are still ahead.

| Before this work | Added |
|---|---|
| Enumeration of loaded actors (verified in game); relay of "NPC entered combat" | Protocol 1.6, server-chosen NPC authority, snapshots, death and damage events, client takeover and release, new engine evidence |

## Technical assessment

**Works and is reused:** the session, transport and worker stack; actor enumeration (`NpcSync.cpp`); the movement-state-1 replay path, proven on player proxies (walk, run, jump); graph-event replay; Papyrus dispatch; hostility relay.

**Does not work or is unproven:**
- Player-proxy aim pitch and third-person body facing are still open (A.13). NPC replication inherits both.
- The replay path has only ever run on a proxy with no packages of its own. Whether it holds against a real NPC's AI is **the main open question** and the first thing to test.

**Main obstacles to complete NPC sync:**

| Obstacle | Why it is hard | State |
|---|---|---|
| Two AIs for one NPC | The AI bit gates the whole actor update, 3D included (RE 12.2), so AI cannot simply be turned off on a client | Movement overridden, decisions still run locally |
| Runtime `FF…` references | Differ per machine, and many encounter NPCs may be these | Excluded. Needs an identity and spawn protocol |
| Saves must match | Data-file matching assumes the same save; a leveled NPC could differ between saves | Not checked by the code at all |
| Quest scripts run on every machine | Local deaths and dialogue can fire script events twice | Not investigated |
| NPCs near a client but not near the authority | The authority can only publish what its engine runs | No answer yet. Those NPCs stay local and unsynced |

## Checklist

### Foundation

| Item | Status | Notes |
|---|---|---|
| Enumerate loaded NPCs | VERIFIED WORKING | 62 of 62 handles resolved, 2026-11-03 |
| Protocol 1.6 messages, validation | VERIFIED (tests) | |
| Server-chosen authority, sticky, skips tool clients | VERIFIED (tests) | |
| Only the authority can publish states or deaths | VERIFIED (tests) | |
| Authority moves when its owner leaves | VERIFIED (tests) | The game-side handover is EXPERIMENTAL |
| Snapshot bounded to 48 nearest NPCs, under 1200 bytes | VERIFIED (tests) | |
| Runtime references refused on the wire | VERIFIED (tests) | |
| Interest management beyond "nearest 48" | UNIMPLEMENTED | Every client gets the same list |

### Movement and animation

| Item | Status | Notes |
|---|---|---|
| Take over a client's NPC, reversibly | EXPERIMENTAL | Vfunc 9 in, vfunc 7 out (RE 12.3) |
| Walk, run, turn from authoritative pose | EXPERIMENTAL | Same path as proxies. Steering maths VERIFIED (tests) |
| Snap on persistent drift | EXPERIMENTAL | Over 3 units for 0.4 s |
| Hand back when the engine takes control (furniture, scenes) | EXPERIMENTAL | Then the NPC runs on local AI, so it can diverge |
| Crowd actors follow the authority's targets | EXPERIMENTAL | Hook on the state machine, position-matched targets (RE 12.10). Decision logic VERIFIED (tests) |
| Crowd inhibition off during a session | EXPERIMENTAL | Frame-rate cost unmeasured |
| Crowd spawn and despawn | UNIMPLEMENTED | Each copy still does its own |
| Weapon drawn state | EXPERIMENTAL | Papyrus, at most once a second |
| Attacks, reloads, hit reactions, staggers on NPCs | EXPERIMENTAL | Graph events captured and replayed. Local events are blocked on held NPCs |
| Sneak, sitting, furniture, ladders, swimming | UNIMPLEMENTED | Sneak and unconscious are published but not applied |
| Aim, look-at, facial animation | UNIMPLEMENTED | Blocked by the same gap as proxies (A.13) |
| Ragdoll, knockback, getting up | UNIMPLEMENTED | |

### Vitals, combat, death

| Item | Status | Notes |
|---|---|---|
| Health percent authority to clients | EXPERIMENTAL | Hit-accounting logic VERIFIED (tests) |
| Client damage forwarded to the authority | PARTIALLY WORKING | Seen in game: 17 hits forwarded and applied. Attacker identity is lost, so no XP or crime credit |
| Death event, replay to late joiners | EXPERIMENTAL | Server side VERIFIED (tests); `KillSilent` on the client untested |
| Combat start relay | PARTIALLY WORKING | Older `NpcCombat`, requires `RemoteAvatars=1`. Its interaction with takeover is untested |
| NPC-on-NPC damage on a client | UNIMPLEMENTED | **Known flaw:** it would be forwarded as if a player did it |
| Shields, staggers, criticals, resistances | UNIMPLEMENTED | Damage arrives as finished health loss |
| Loot, respawn, resurrection | UNIMPLEMENTED | Loot is rolled per machine. `Resurrect` is not handled |
| Essential or protected NPCs | UNIMPLEMENTED | Left to the authority's engine, untested |

### Appearance and equipment

| Item | Status | Notes |
|---|---|---|
| Same reference shows the same appearance | UNIMPLEMENTED | Assumed from the shared save, **not verified**. No base or appearance check on the wire |
| Equipment replication | EXPERIMENTAL | Changes only, through `TESEquipEvent`. Assumes the same starting save. Layout from CommonLibSF, unverified on this runtime |
| Runtime-spawned NPCs | UNIMPLEMENTED | |

### Dialogue, quests, world

| Item | Status | Notes |
|---|---|---|
| Local player can talk to a taken-over NPC | EXPERIMENTAL | The NPC is released during dialogue |
| Shared dialogue, voice, lip sync, choices | UNIMPLEMENTED | Entry points surveyed (RE 12.4), internals not investigated |
| Quest stages, aliases, shared vs personal progress | UNIMPLEMENTED | Entry points surveyed (RE 12.4), internals not investigated |
| Doors, elevators, containers, physics, hazards | UNIMPLEMENTED | Not investigated |
| Cell transitions | EXPERIMENTAL | Location mismatch leads to release after 5 s |
| Late joining | PARTIALLY | Snapshots and deaths arrive; dialogue, quest and world state do not |

## In-game run log

### 2026-11-04, first build, two copies, New Atlantis

| Measure | Result |
|---|---|
| Connection, authority | Worked. Copy 1 published about 10 snapshots a second |
| NPCs loaded on copy 1 | 61 to 62, but at most 48 published |
| Takeover | 48 of 48 taken over with 0 losses at the first check, about 8 s in. 72 losses by the next check, 10 s later |
| After that | 412 releases for leaving the state: 328 into state 4, 20 into state 0, 64 into state 2. Some within 50 ms of takeover |
| Hits forwarded | 17 on one NPC, attacked on purpose from copy 2, all applied on copy 1. Matching health on both screens was not confirmed |
| Shutdown | Both copies closed by the user, not a crash |
| Other-location parts | 206 ignored. NPCs across a cell border were being dropped |
| User report | "Only a handful" of NPCs synced. All NPCs are required, including random ones walking the planets |

**What it proved:** the takeover works on real NPCs, but it does not hold: their AI requests other movement states. Coverage lost NPCs at the 48 cap, on runtime `FF…` references, and at the location check.

**Changes for the second build (not run):** controller hold (RE 12.3), snapshots across several messages with up to 512 NPCs, 256 takeover slots, runtime references matched by FormID and base, no location gate, and a scan of `ProcessLists` for other actor lists.

**Known remaining gaps in coverage:**
- A runtime NPC that exists only on the authority is counted as unmatched and is not spawned on the client. Client-only NPCs are not removed either. The new log line measures how many there are.
- NPCs loaded only around a client, away from the authority, are not synced.

### 2026-11-04, second build, two copies, New Atlantis, about 15 minutes

| Measure | Result |
|---|---|
| Control losses | **0**, down from 412. 53 of 58 NPCs held. No crash |
| State changes refused | 16,400 in total, 14,079 of them requests for state 4 or 9 |
| Copy 1 | 64 of 64 NPCs in its main list published, 6 of them runtime |
| Copy 2 | 6 to 7 runtime NPCs unmatched, about 5 NPCs never taken over |
| Actor lists | Main list `+0x60` 64, `+0x70` 1173, `+0x90` 35, `+0x190` 5 |
| User report | NPCs still not all in sync. The game should not pause in menus |

**Third build (not run):** NPC graph events captured on the authority and replayed; local graph events blocked on held NPCs; equipment changes through `TESEquipEvent`; sitting NPCs released to local AI; other actor lists included when an NPC has 3D; menus no longer pause during a session; unmatched and busy NPCs logged by ID.

## Test plan

Automated: `./scripts/build.ps1` (100 tests, last run 2026-11-04: all passed). These cover the protocol, server routing and replication decisions. They do not touch the game.

In game, two copies on one PC, both loading **the same save**:

```powershell
./scripts/deploy-plugin.ps1
./scripts/launch-instances.ps1 -Avatars -AvatarMoveMode controls -NpcSync
```

Copy 1 is the authority. Stand in a populated spot such as New Atlantis. Logs: `Documents/My Games/Starfield/SFSE/Logs/CoSyncSF-p<n>.log`, lines tagged `[npc]`.

Run these in order, and stop at the first failure. Each one depends on those above it.

| # | Do | Pass means | Result |
|---|---|---|---|
| 1 | Load both copies, wait 30 s | No crash. p1 logs `authority: … snapshots published`, p2 logs `replica: …` with a rising snapshot count | not run |
| 2 | Read p2's `take over` lines | Registry shows `CoSync-replay#`, state goes `2 -> 1`. Control losses stay near 0 | not run |
| 3 | Watch one walking NPC in both windows | Same NPC, same place, walking animation, no sliding, few snaps | not run |
| 4 | Watch for 2 minutes | NPCs do not fight the takeover: no vibrating, no pile-ups | not run |
| 5 | Shoot an NPC from copy 2 | Copy 1 logs `hit … applied`, health matches in both | Partial: hits applied (2026-11-04), health match not confirmed |
| 6 | Kill that NPC from copy 2 | Dies once in both windows, stays dead, no crash | not run |
| 7 | Kill one from copy 1 | Dies in copy 2 | not run |
| 8 | Talk to an NPC from copy 2 | Dialogue opens and the NPC stops following the authority | not run |
| 9 | Move copy 2 to another cell and back | `release` on leaving, takeover on return, no crash | not run |
| 10 | Leave the session from copy 2 and keep playing there | `release` lines; NPCs behave normally on their own | not run |
| 11 | Rejoin, then close copy 1 | The host's session ends: p2 logs `release (disconnected)` and NPCs carry on | not run |
| 12 | Run `cosync-server`, point two or three copies at it with `Mode=client`, close the first | The next copy logs `is now the NPC authority`, NPCs it had taken over resume on their own AI | not run |
| 13 | On that server, join a further copy after a kill | It sees that NPC dead | not run |

Test 12 is the only way to see a handover: normally the session server lives inside copy 1, so closing it ends the session.

Also record frame rate with and without `-NpcSync`: per-frame cost with many NPCs has not been measured.

## Next stages

See [NPC_SYNC_ROADMAP.md](NPC_SYNC_ROADMAP.md). The next step is running the test table above: whether the takeover survives real AI decides everything after it.
