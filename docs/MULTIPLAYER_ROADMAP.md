# Multiplayer Roadmap

The whole road to a shared Starfield, ranked. Status words are defined in [WORLD_SYNC.md](WORLD_SYNC.md). Release order: test on `SF-Dev` in game, then CI, then merge.

## The model

Every copy runs the full game from the same save, and CoSync keeps those copies agreeing. It is not a server simulation: no copy can run headless, and the engine has no second player (RE 6). This decides most of what follows:

- A world change is **applied through the engine** on each copy, not simulated centrally.
- Data-file form IDs are stable across copies. **Runtime FF IDs are not**, so anything runtime needs proof of identity.
- Anything random diverges by default (RE 12.10), so what matters is replicated as the **decision**, and the engine carries it out.

## Existing

| Feature | Status | Notes |
|---|---|---|
| Sessions: host, join, resume, groups, Steam invites | Two copies | |
| Remote player movement, animation, weapons, status | Two copies | Through replayed controls |
| NPC position, health, death, damage, outfits | Two copies | One authority per session |
| Ambient crowd follows the authority | Two copies | Median 75 seated on both, was 27 |
| Doors, world pickups, data-file containers, clock, weather | Two copies | New, 2026-11-07 |
| Quest stages and quest start | Two copies | New. Quests a guest starts stay local |
| Locks | Two copies | New |
| Shared changes survive loading a save | Two copies | New. Container counts excluded |
| Ship position in space reaches other players | Two copies | New. Showing it as a stand-in is blocked, see [SHIP_SYNC.md](SHIP_SYNC.md) |
| Presence: NPC updates only to players who can see them | Two copies | New. `cosync/shared/Presence.h` |
| Late join for world state | Two copies | New |
| Unattended two-copy test harness | Working | Loads a save, runs steps, reads back |

## Priorities

Rank combines importance, feasibility, dependencies and regression risk.

| # | Work | Why now | Depends on | Risk |
|---|---|---|---|---|
| 1 | Decide what a guest-started quest does, and test rewards | Stages work, the policy is the gap | | Low |
| 2 | Crowd spawn and despawn (#2) | The largest remaining NPC divergence | | Medium, needs hooks |
| 3 | Runtime reference identity | Unblocks runtime containers, dropped items, spawned loot | | Medium |
| 4 | Item transfers arbitrated by the server | Prevents duplication | 3 | Medium |
| 5 | Presence beyond NPCs: world events, ships, combat by place | NPC traffic is filtered by place already | | Low |
| 6 | Reconnection and save/load through a session | A session must survive normal play | | Medium |
| 7 | Projectiles, hits and damage between players | Combat is cosmetic until this | 5 | High |
| 8 | Dialogue and scenes: one speaker, others watch | Quests depend on it in practice | 1 | High |
| 9 | Companions and crew | | 8 | Medium |
| 10 | Ships: shared interior, then flight | Starfield's defining feature, largest unknown | 5, 6 | High |
| 11 | Outposts, crafting, research | Mostly per-player or container flows | 3, 4 | Medium |
| 12 | Destruction, physics objects, locks | `SetDestroyed`, `Lock` are exposed; physics is not | 5 | Medium |

## Multiplayer requirements discovered

The list below comes from asking what Starfield assumes about having one player.

- **Reference identity is the central dependency.** Spawned NPCs, dropped items, placed ships and outpost objects all have per-copy IDs. Without a mapping they cannot be named across copies.
- **Quests assume one actor.** A stage set on a quest that is not running starts it, and fragments run once per copy. Shared stages must therefore never start quests, and fragments that grant rewards can pay out on each copy.
- **Credits and rewards are per-copy.** A quest reward lands on every copy's player. That may be right for co-op, and it is a decision to make.
- **Loading is per-player.** Doors already wait for their cell, and every apply must assume its target is not loaded.
- **Time passes during menus and loads.** The clock needs its authority, and the host's sleep or wait moves every player's clock.
- **Hostility and crime are per-copy.** An attack on one copy must become a decision on all.
- **Ship travel moves a whole world.** A grav jump unloads everything, so a passenger must follow the pilot or be left behind. Events exist, the policy does not.
- **Disconnect during an interaction.** A player leaving mid-dialogue or mid-transfer needs a defined outcome.
- **Fast travel and interiors** need the same presence rule as ships.

## Confirmed engine findings

Addresses, offsets and evidence are in RE 12.10 and 12.11.

| Finding | Value |
|---|---|
| Global event getters | IDs 64095 to 64237, 142 sources, all sinkable. A generic sink reads any of them |
| `TESContainerChangedEvent` | ID 64118, payload `from, to, item, count`. Runtime proven |
| Quest stage events | IDs 64160 and 64161, `quest, stage` |
| Door open and close | ID 64153, `ref, activator, flag` |
| Door state | `0x5CDC70`: 1 open, 2 opening, 3 closed, 4 closing |
| Console entry | `0x1EA1E10`, global `0x61E26B0`. Runs `LoadGame` from the main menu |
| Cell references | Array data at cell `+0x88` |
| Ship events in CommonLibSF | Take-off, landing, dock, grav jump, far travel, damage, have real IDs |
| Ship natives | `TakeOff`, `InstantDock`, `InstantUndock`, `PlaceShipAtMe`, `GetDockedShips`, `IsLanded` |
| Quest flags | `quest+0x114`: bit 0 running, `0x80` stopping, `0x800` tracked |
| Quest objectives and stages | `+0x148` count, `+0x150` objectives, state at `+40`. `+0x138` count, `+0x140` stages, index at `+16`, done at `+18` |
| CommonLibSF IDs of 0 | `TESQuest::IsStageDone` is 0 here and calling it froze the game. The real ID is 68530. Check any helper's ID before use |

## Blocked or unknown

| Item | What is missing |
|---|---|
| `TESDataHandler::formArrays` | Reads as empty for quests on 1.16.244. Quests are found by scanning `LookupByID` instead |
| Cause of the 2026-11-07 crash | Walking `references` the plain way read a pointer of `2`. The array location was correct, so the fault is elsewhere and unexplained |
| `set gamehour` from the console entry | No effect, cause unknown |
| Spawn and despawn suppression | Needs hooks at `0x1276A70` and `0x1281250` |
| Ghidra | Not used: IDA covered everything so far |
| Physics, projectiles, ship simulation | Not investigated beyond script APIs |
| Two machines | Needs a second PC |

## Investigation coverage

Named targets from the brief, by how far they have been taken.

| Depth | Systems |
|---|---|
| Synced and tested | Doors, containers, world items, clock, weather, quest stages, crowd, NPC movement, animation, equipment, death |
| Entry points known, not synced | Dialogue, scenes, ships, locks, destruction, outposts |
| Script API surveyed only | Crew, factions, crime, crafting, research, fast travel |
| Not investigated | Havok, projectiles, damage calculation, AI packages, world streaming, spaceflight simulation, camera |
