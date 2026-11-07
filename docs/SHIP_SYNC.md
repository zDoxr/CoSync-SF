# Ship Sync

Ships in space, issue #10. Status words are the ones in [WORLD_SYNC.md](WORLD_SYNC.md). Engine evidence is in [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) §12.11.

## The problem

Every player runs the same save, so every player has the same home ship. Another player's ship cannot be "their ship" on this copy: that reference is this player's own. It has to be shown as a **stand-in** of the same design, placed where the other player's ship is.

## State

| Part | Status | Evidence, 2026-11-07 |
|---|---|---|
| Reading the local ship in space | **Two copies** | Ship `0003F7E8` read with `IsInSpace` true for ship and player |
| Sending it: protocol 1.12, transform section 5 (cell, design, position, angle) | **Two copies** | Each copy received the other's ship 30 times a second |
| Same space cell recognised on both copies | **Two copies** | Both reported cell key `5C12A803` for `00018343` |
| Movement reaches the other copy | **Two copies** | Copy 1 moved its ship 500, then 2000 units. Copy 2 read 500.0, then 2500.0 apart, within 10 s |
| Two players inside one ship | **Two copies** | Both boarded the same ship in space. Each copy showed the other player inside, following them between seats, settled within 0.64 units |
| The shared home ship follows when another player moves it | Implemented | Applies the other player's ship transform to this copy's home ship. Moving a **landed** ship this way does nothing: its position stayed put. Moving one in space worked |
| Placing the stand-in ship | **Blocked** | `PlaceShipAtMe` needs the local player to be flying, see below |
| Moving the stand-in each frame | Not started | Depends on placing it |
| Taking off and landing unattended | **Blocked** | See below |

## Testing without flying

A copy of a save in which the home ship is parked in space (one of the user's autosaves, never the original) boards straight into space:

```powershell
./scripts/launch-instances.ps1 -Avatars -AvatarMoveMode controls -NpcSync -WorldSync -AutoLoad CoSyncShip `
    -TestCommandDelay 50 -TestCommands '@home;@board;@ship;@shiftship 500;@ship' -TestCommands2 '@home;@board'
```

`@home` asks the game for the home ship, `@board` moves the player aboard, `@ship` logs both, and `@shiftship <dx>` moves the home ship as flying would. Each copy logs `[ship] player N flies ...` with the distance between the two ships.

## What blocks the rest

- **Placing a stand-in.** In space `PlaceShipAtMe` takes a separate path. With the player aboard but not flying, it put the new ship into the galaxy cell, disabled, with no 3D, and the script call returned None. Its own error text says this happens when "the player wasn't in a ship". The player has to be at the controls.
- **Getting to the controls unattended.** Sitting in the pilot seat by script works on the ground. In space the seat lets go at once, and the event census shows an activation and a furniture exit. Taking off fails the same way: `tots`, `TakeOff`'s core and the Launch button's handler all leave the ship landed.

Both need a person at the controls once. A play-test of one player flying while the other watches would show whether the stand-in then places.

## Known limits

- Moving a ship from a script with `SetAngle` dropped its pitch and roll (1.57, -1.31 became 0, 0). Real flight does not go through that path.
- Ship state is sent with every transform. A dedicated message at a lower rate would cost less, and can come once the stand-in exists.
