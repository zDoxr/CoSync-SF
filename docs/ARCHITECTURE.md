# CoSync-SF — Architecture

## Components

```
src/
  CoSyncSF.Shared/   protocol messages, serialization, validation, runtime versions,
                     logging, bounded queues. No OS/transport/game dependencies.
  CoSyncSF.Network/  ITransport + backends (GnsTransport, LoopbackTransport),
                     SessionServer, SessionClient, NetworkWorker (thread owner),
                     LAN discovery. No Starfield code.
  CoSyncSF.Server/   cosync-server.exe — standalone session server process.
  CoSyncSF.Client/   cosync-testclient.exe — headless protocol client for LAN tests.
  CoSyncSF.Plugin/   CoSyncSF.dll — SFSE plugin: config, logging, game-thread bridge.
tests/CoSyncSF.Tests/  GoogleTest suite (unit + loopback UDP integration).
```

Dependency direction: `Plugin → Network → Shared`, `Server → Network → Shared`. Nothing depends on the Plugin. The session layer has no idea it is inside Starfield.

## Transport boundary

```
            ┌──────────────── ITransport ────────────────┐
 Session    │ Listen / Connect / Send(Reliable|Unreliable) │
 layer ───► │ Close(code, linger) / Poll(events)           │
            └──────┬───────────────┬───────────────┬───────┘
                   │               │               │
            GnsTransport   LoopbackTransport   SteamTransport (planned)
            (LAN/direct)   (tests, in-memory)  (Steam P2P/SDR, identity)
```

- Events: `Connected`, `Disconnected(code, reason, closedLocally)`, `Message(bytes)`.
- `Close()` always produces a `Disconnected` event, so there is one cleanup path.
- Linger semantics: a `Reject` sent before `Close(linger=true)` is delivered before the peer sees the close. Tested over real GNS.
- Size limits are enforced inside the transport (GNS `RecvMaxMessageSize`) and again by the decoder.

## Wire protocol (v1.0)

`u16 type | u16 flags(=0) | payload`, little-endian, strings u8-length-prefixed strict UTF-8 with per-field limits, exact-length decoding (trailing bytes rejected), max message 16 KiB.

| Type | Dir | Delivery | Purpose |
|---|---|---|---|
| Hello | C→S | reliable | magic, protocol major/minor, game runtime, build, name, password, resume token |
| Welcome | S→C | reliable | player id, session id, new resume token, server name, limits, heartbeat interval |
| Reject | S→C | reliable | reason enum + detail, then close |
| SessionSnapshot | S→C | reliable | roster after Welcome |
| PlayerJoined / PlayerLeft | S→C | reliable | roster deltas (leave reason incl. `ConnectionLost`) |
| Heartbeat / HeartbeatAck | C↔S | reliable | liveness + RTT |
| Disconnect | both | reliable | graceful quit / shutdown / kick |
| PlayerTransform | C→S→C | unreliable | seq, opaque locationId, position, rotation |

**Version negotiation:** the first 8 payload bytes of `Hello` (magic, major, minor) and the whole `Reject` layout are frozen forever. A server can therefore always read a future client's version and reply with a `ProtocolVersionMismatch` Reject that the client can decode. Minor versions may append fields.

**Validation (server):**
- The first message must be `Hello` within 5 s.
- At most 16 pending handshakes.
- Token-bucket rate limit (120 msg/s, burst 240).
- Server-to-client message types are rejected.
- A second `Hello` is a violation.
- Transforms must be finite and bounded and have a newer sequence number (stale ones are dropped).
- A non-zero `playerId` that is not the sender's own counts as spoofing; the server always stamps the authenticated id.
- Name rules: 1–32 bytes, case-insensitive unique.
- Optional password, compared in constant time.
- Optional exact game-runtime match.

## Session lifecycle

```
Client: Idle → Connecting → Handshaking → InSession ─┬─ Disconnect() → Disconnected
                    ▲                                ├─ Reject / Kick / server shutdown → Disconnected
                    └──── Reconnecting ◄─────────────┘ unexpected drop (holds resume token)

Server, per player: Active ──drop/timeout──► Reserved (resumeWindow, default 30 s) ──expiry──► removed
                      ▲                          │
                      └──── Hello + valid token ─┘ (same PlayerId, token rotated)
```

- A drop broadcasts `PlayerLeft(ConnectionLost)`. A resume broadcasts `PlayerJoined` with the **same id**.
- If a half-open old connection still exists when the resume arrives, it is replaced (`CloseCode::Replaced`).
- Graceful quit, kick and protocol violations remove the player immediately (no resume).
- Player ids are never reused within a session; `SessionId` changes on every server start.

## Threading model (plugin)

```
 Starfield main thread                         CoSync network thread (NetworkWorker)
 ─────────────────────                         ─────────────────────────────────────
 SFSE AddTaskPermanent → GameBridge::Tick()    owns GnsTransport(s), SessionServer (host),
   DrainEvents(≤256/frame)  ◄── BoundedQueue ── SessionClient, discovery responder
   TakeRemoteTransforms()   ◄── LatestValueMailbox (newest per player)
   SubmitLocalTransform()   ──► single-slot mailbox ──► sent once per tick
   ConsumeOverflow() → rebuild from Roster()
```

Rules:
1. **Engine objects are touched only on the main thread**, inside `GameBridge::Tick()`. All CommonLibSF/engine code is in `src/CoSyncSF.Plugin/src/GameState.cpp`, and that file is never called when the Address Library is missing.
2. **Network objects are touched only on the network thread.** The game thread never blocks on I/O.
3. All cross-thread channels are bounded: the event queue has a fixed capacity and sets an overflow flag (with resync from roster), and the transform mailboxes are O(players).
4. GNS callbacks only queue plain-data copies for their owning transport.
5. Shutdown: `NetworkWorker::Stop()` sends a graceful Disconnect, flushes for about 200 ms, then joins. The plugin never joins from `DllMain`, which would deadlock on the loader lock. On game exit the OS ends the thread and the server's heartbeat timeout reclaims the slot.

## Authority model (proposed)

| State | Authority | Notes |
|---|---|---|
| Session membership, identity, player ids | Session server (standalone or in-host) | implemented |
| Own player transform/animation | Owning client; server validates bounds/rate | relay implemented, game side pending |
| Own inventory/equipment | Owning client, server-validated events | Phase C |
| Damage between players | Victim's client applies, attacker's client proposes; server validates rate/range | Phase C |
| NPCs, containers, world objects in a cell | One "cell owner" client (host first, later per-cell ownership) | Phase C/D |
| Quests/progression | Host | Phase E |

Long term, the session server can become more authoritative for state it can validate without simulating Starfield (inventory ledgers, loot rolls, persistence). Physics/AI authority stays with a game client unless model 1 (see RESEARCH §7) becomes possible.

## Entity identity (design)

- `PlayerId` (u32, server-assigned) for players.
- Synced world entities will get a `NetEntityId` (u32, owner-assigned within a server-granted range), with an explicit spawn/despawn lifecycle and per-entity sequence numbers for stale-update rejection.
- Game references are mapped as (plugin file name, local FormID) → never raw FormIDs across machines, because load orders differ.

## Engine integration: remote-player proxies

Evidence: [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md), [REMOTE_PROXY_RESEARCH.md](REMOTE_PROXY_RESEARCH.md).

```
 network thread                  main thread (GameBridge::Tick)                       Papyrus VM threads
 ──────────────                  ─────────────────────────────                        ──────────────────
 PlayerTransform ─► mailbox ─►   interpolator.Sample()
 PlayerJoined/Left ─► queue ─►   per remote: Avatar state machine
                                   none ── same cell & VM settled ──► spawn requested ──► PlaceActorAtMe (engine spawn path)
                                   spawned ◄── atomic result slot ◄──────────────────────── callback (FormID / error)
                                   spawned: Probe() (sync, verified offsets) ─ log VALID/INCOMPLETE
                                            Move() 10 Hz ───────────────────────────────► SetPosition / SetAngle
                                   leave/cell change: Despawn() ────────────────────────► Disable + Delete
                                            re-Probe after 2 s (must be gone)
```

- The proxy is an **NPC actor created by the engine's own `PlaceActorAtMe` → `TESDataHandler::CreateReference` path**. It is not a second PlayerCharacter, because the engine has no pathway for one (RE §6).
- Engine reads in `Avatars.cpp` are limited to offsets and virtuals proven in the exe (RE §4). Anything else goes through engine natives.
- VM results come back on VM threads and are published only through `std::atomic` slots. The main thread is the only consumer and the only engine caller.
- Proxies are bounded to the local player's cell. They are despawned on leave, on cell change and at session end. Results of in-flight spawns that arrive late are despawned (`_orphans`).

## UI layer (planned)

See [UI_FRAMEWORK.md](UI_FRAMEWORK.md). It is a Scaleform AS3 view (`CoSyncMultiplayerMenu.swf`) fed by an immutable `UiSnapshot` from a C++ `UiModel`. The view emits `UiCommand` intents, which are validated, then forwarded to `NetworkWorker` or queued for `GameBridge`. UI code never calls engine code or sockets directly.

## Module boundaries vs. the mission's proposed layers

| Proposed layer | Where it lives today |
|---|---|
| Core (lifecycle, config, logging, version) | `CoSyncSF.Plugin/Plugin.cpp`, `PluginConfig.*`, `CoSyncSF.Shared/Log`, `GameRuntime` |
| Networking (GNS, sessions, identity, serialization) | `CoSyncSF.Network`, `CoSyncSF.Shared/Protocol` |
| World (actor lifecycle, proxies, cells) | `CoSyncSF.Plugin/Avatars.*` (engine calls), `GameBridge` (lifecycle) |
| Player (local tracking, remote representation) | `GameState.*` (local), `GameBridge` + `Avatars` (remote) |
| Synchronization (interpolation, authority) | `CoSyncSF.Shared/Interpolation`, server relay/validation |
| UI | planned (`UI_FRAMEWORK.md`) |

No new modules were introduced. The current split already keeps networking free of engine code. `Avatars.*` should become a `World/` folder once it grows past proxies.
