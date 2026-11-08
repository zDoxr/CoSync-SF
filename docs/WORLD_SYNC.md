# World Sync

Shared world state: what one player changes, the others see. Off unless `[Experimental] WorldSync=1` (`launch-instances.ps1 -WorldSync`). Protocol 1.11. Engine evidence is in [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) §12.11.

Status words, used strictly:

- **Implemented**: written, compiles.
- **Unit tested**: automated tests pass, no game involved.
- **Two copies**: run in two real game copies on one PC, and the receiving copy's engine confirmed the result.
- **Verified**: also played by a person, or across two machines. Nothing here has reached this yet.

## State of each system

| System | Status | Evidence, 2026-11-07, Starfield 1.16.244 |
|---|---|---|
| Doors | **Two copies** | Door `000B2B04`: sender state 1 to 3 and 3 to 1. Receiver read back engine state 3 and 1 |
| Things taken from the world | **Two copies** | `00229BAF` taken on copy 1, read back as gone on copy 2 |
| Data-file containers | **Two copies** | `0022ED8A` emptied: 111 stacks reported, 111 applied, 111 reported lost by the receiver's own engine |
| Game clock | **Two copies** | Host's hour 10.1 to 15.0, receiver stepped within 1 s. Hour 21 reached a late joiner |
| Late join | **Two copies** | Copy 2 joined 2 minutes after the changes. Door, removal and clock all arrived, 0 dropped |
| Quest stages | **Two copies** | `00352CBA` stage 200 set on copy 1, read back as done on copy 2 |
| Quest start | **Two copies** | `00193266` started on copy 1, then running with stage 5 on copy 2 |
| Weather | **Two copies** | Host changed `001034AB` to `0002B07E`, copy 2 followed within 3 s |
| Loading a save mid-session | **Two copies** | Copy 1 unlocked `001661A7`, then loaded its save, which has it locked. 5 s later the change was applied again and the engine read back unlocked. The session stayed up |
| Locks | **Two copies** | `001661A7` unlocked then locked on copy 1, copy 2 read back engine state 0 then 1 |
| Destroyed objects | Implemented | Apply and read-back written. The engine event is not yet identified, and no breakable object was found in the test save |
| Runtime (FF) containers | Unit tested | Deliberately not shared, see below |
| Runtime NPC inventories (#6) | **Two copies** | Copy 1 took `0031144F` from crowd NPC `FF01127D`. Copy 2 applied it to its own paired NPC `FF011200` 0.04 s later, and its engine reported that NPC losing the item. Protocol 1.16 `NpcContainerItem`, named by the NPC authority's number |
| Dropped connection and resume (#14) | **Two copies** | Copy 2 dropped its connection for 20 s (`@netdrop 20`) and resumed as the same player. Copy 1 unlocked `001661A7` meanwhile, and copy 2 read it back as unlocked 3 s after resuming. The 111 container changes copy 2 already had came back with the history and were all skipped as applied: 111 applies, not 222 |
| Dropped items (#6) | **Two copies** | Copy 1 dropped `00066800` x2 as `FF018507`, named shared `01000001`. Copy 2 placed its own `FF017E0A` at the same position (-783.6, 1637.2, -160.4) 0.08 s later. Copy 2 picked it up, and copy 1 removed its drop, read back as gone |
| Two players emptying the same container (#17) | **Two copies** | Copy 2 held incoming events and emptied `0022ED8A` 10 s after copy 1 did. It had 5000 credits and 115 inventory entries, then the server answered with 110 give-backs, and it was back to 0 credits and its own 6 entries. Others only heard copy 1's take |
| Two players taking the same item (#17) | **Two copies** | Copy 2 held incoming events (`@holdworld`) so both took `00066808` before hearing of the other. Copy 1 was first, and the server answered copy 2 with "give back `00066800` x1". Copy 2's engine then reported "player lost `00066800` x1", and its count went from 1 to 0 |

Crowd sync, measured in the same runs and over 16 minutes: seated on both copies, median 75 (was 27), and seated on one side, median 15 (was 71). No faults. Followers start at 86 of 104 and fall to about 64 as the copies' crowds diverge, which is issue #2.

## How it works

**Observing.** A sink on `Activation::Event` notes what the local player activated. For a door or a loose item, a watch of up to 4 s reads what the engine actually did. A locked door never changes state, so nothing is sent. Container and quest changes come from their own sinks. Sinks run on any thread and only queue numbers, and all engine reads happen in `Tick` on the main thread.

**Applying.** Through the existing Papyrus dispatch: `SetOpen`, `DisableNoWait`, `AddItem`, `RemoveItem`, `SetCurrentStageID`, `Lock`. Weather forms have no script object to call, so weather uses the engine's `fw` command. Each apply is read back from the engine after 3 s and logged as `[world] verify ...`.

**Authority.**

| State | Who decides | Conflict rule |
|---|---|---|
| Doors, removed references | The player who acted | Newest report wins, one ledger entry per reference |
| Locks | Whoever's engine changed it | Newest report wins. A lock event that changes nothing is not sent |
| Containers | The player who acted | Counts add up. Two players taking the same stack can still both succeed: see limits |
| Quest stages | Any player, quests with an objective on show | Stages only accumulate. Only the authority's report may start a quest on another copy |
| Clock, weather | The NPC authority | Others step only past 2 game minutes, so they do not chase small differences. Weather only applies in the same location |

**Late join.** `world::Ledger` on the server keeps one entry per changing thing, so a door opened fifty times costs one entry. A joiner receives the ledger in order, then the latest clock. It holds up to 16,384 entries. When full it refuses new ones and counts them in `worldEventsNotRemembered`.

**Echoes.** An applied quest stage makes the engine report that stage again, so `world::EchoFilter` swallows it once. Container applies do not involve the player, so they cannot come back. A door apply has no player activation.

## Known limits

- **Runtime containers stay local.** FF references are numbered per copy, and a container event carries no base ID to prove identity. This was a real bug, found in the late-join test: `FF010C54` was sent and applied to whatever had that number on the other copy. It is now refused at both ends. Refusal path unit tested, not exercised in game.
- **Locks follow the engine, not only the player.** The lock event (RE 12.11) also fires when an NPC or a script changes a lock, and those changes are sent too. On a busy door in New Atlantis something unlocked a door a second after the test locked it, and that unlock was sent as well. The cause was not confirmed. A script that locks the same door on every copy costs nothing: the receiver already has that state and does nothing.
- **Doors wait for their cell.** A door whose cell is not attached is retried every 2 s until a player brings it in.
- **Dropped items (protocol 1.16).** The inventory event (ID 64118) names the reference a drop made, at `+16`, found with the event census. That reference is not in the cell's reference list, and runtime numbers come with gaps of tens of thousands, so neither could find it. The dropping player names the drop with a shared ID (their player ID in the top byte), and every copy in the same location places one: the item is added to the player and dropped, which is kept out of that copy's own reports, then moved into place. Picking it up anywhere sends `SharedRemoved`, and the server keeps drops for players who join later until then. After a load, drops whose reference went are placed again. Late join, two copies: copy 2 joined about 80 s after copy 1 dropped, placed the drop at the same position, picked it up, and copy 1 read its own back as gone. Reload, two copies: copy 2 loaded its save after placing the drop, placed it again 6 s after it was back in the world (FF018052, then FF01916C, same position), picked it up, and copy 1 read its own back as gone. Not yet tested: a drop in a location the other player is not in. In the test, picking up the stack of 2 by script gave copy 2 only 1.
- **Things taken from the world go to whoever was first (protocol 1.15).** Each removal now says how many the player got, from the inventory event. The server keeps who took each reference (`world::Pickups`). A later report from someone else is not passed on, and that player is sent `ItemReturned`, which removes what they picked up and shows "Another player took ... first". Unit tested, and run on two copies as above.
- **Taking an item by script from a distance does not really add it.** The harness activates the item from where the player stands. The engine raises "player gained x1" and removes the item from the world, but the count stays the same: 3 before and 3 after, read both by `GetItemCount` and from the engine's inventory list. Counting itself is right: `AddItem` 250 credits read back as 250. So the give-back test gave copy 2 one of the item first, and the give-back removed it. A player picking an item up by hand has not been checked against this. If that also fails to add it, the give-back would take an item the player already had.
- **Containers: what was already gone goes back (protocol 1.17).** Each container change is followed by `ContainerRemaining`, what the container holds of that item on the sender's copy now. The server (`world::ContainerStock`) knows what the last report left. A take that found more than that was taken twice, and the excess is answered with `ItemReturned` and taken off what the others hear. The first report on a container has nothing to compare with. A container restocked by the game between two reports would be read as a stale take.
- **Ambient quests are local by design.** About 1 stage event per 2 s, all left on their own copy. A quest counts as shared when it has an objective on show, the same test as `Quest.IsObjectiveDisplayed`. `Quest.IsActive` proved to mean only the one tracked quest.
- **A quest a guest starts stays on the guest's copy.** Each copy also picks some quests at random (`00193266`, `00193269`, `0019326B`, stage 5, in one test). If all of those spread, a long session would pile them up, so only the authority starts quests elsewhere. This also holds back a quest a guest really did pick up. The history a late joiner receives has no sender and counts as the authority's.
- **Quest rewards can pay out on every copy.** A stage's script fragments run on each copy that applies it. Not tested.
- **Tested with two copies on one PC only.** Not across machines, not with real latency, and only in the `CoSyncTest` New Atlantis save.
- **Resuming a dropped connection.** The server sends the whole history again, as to a late joiner. Container entries are session totals, so a resumed copy applies only what its own record does not have yet (`world::MissingContainerChange`). Changes this copy makes while the connection is down wait in the queue and go out once it is back, instead of being lost. A copy that is disconnected for good starts again from nothing, and joining the same session again then would apply container totals a second time.
- **After a load, container counts stay as the save has them.** Other changes are applied again, but a count added to a save made after the change would hand out items twice.
- **Not covered by a test:** reconnection, three or more players, simultaneous actions.

## Running the automated test

Unattended, from a shell. Copy 1 runs the steps, and each copy's log shows the result.

```powershell
./scripts/deploy-plugin.ps1
./scripts/launch-instances.ps1 -Avatars -AvatarMoveMode controls -NpcSync -WorldSync -AutoLoad CoSyncTest `
    -TestCommandDelay 50 -TestCommands '@ref 000B2B04;@near 000B2B04 item;@near 000B2B04 take;@ref 000B2B04;@time 15'
```

Add `-DelaySeconds 170` to make copy 2 a late joiner, and `-EventCensus` to log which engine events fire after each step. Read `[world]` lines in `CoSyncSF-p1.log` and `CoSyncSF-p2.log`, then `Stop-Process` the copies when done. `set gamehour` through the console has no effect from here, which is why `@time` exists.
