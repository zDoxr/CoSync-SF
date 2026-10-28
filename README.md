# CoSync-SF

Cooperative multiplayer and dedicated session hosting for **Starfield** (PC), built as an SFSE plugin plus a standalone session server.

> **Status: early foundation.** The networking layer, session server and protocol are implemented and tested over real UDP on one machine. **No game state is synchronized yet.** The plugin has not been loaded in Starfield yet. See [Current status](#current-status).

## Intended experience

Two to eight players join one session, over LAN by direct address or discovery, and later over Steam. Each player sees the others as characters in their world. Combat, loot and world state are shared, and an optional standalone server keeps sessions and characters persistent. Getting there is a long road; [docs/ROADMAP.md](docs/ROADMAP.md) lists the milestones honestly.

## Current status

| Area | State |
|---|---|
| Wire protocol v1.0 (handshake, version negotiation, registration, heartbeat, resume, roster, transforms, disconnect) | Implemented, unit-tested |
| Transport abstraction + GameNetworkingSockets backend (LAN / direct IP) | Implemented, tested over UDP loopback |
| Session server: validation, rate limits, resume window, transform relay | Implemented, tested |
| LAN discovery (UDP query/response) | Implemented, tested over loopback unicast |
| `cosync-server` (standalone session server) | Implemented. It does **not** simulate Starfield. |
| `cosync-testclient` (headless protocol client) | Implemented |
| SFSE plugin (`CoSyncSF.dll`, Starfield 1.16.244) | Builds; starts networking after data load and logs session events. **Not yet tested in game.** |
| Steam networking, NAT traversal | Researched, not implemented |
| Player/NPC/world synchronization | Not started (next milestone) |

## Architecture

```
CoSyncSF.Plugin  ──►  CoSyncSF.Network  ──►  CoSyncSF.Shared
(SFSE DLL,            (ITransport, GNS,      (protocol, serialization,
 game thread)          sessions, worker)      validation, queues)
CoSyncSF.Server ──┘   CoSyncSF.Client (test client)
```

- **Networking never touches engine objects.** It runs on its own thread. The game thread drains a bounded event queue and a latest-value transform mailbox once per frame (SFSE permanent task).
- **The server is a session authority**, handling identity, membership, validation and relay. Game simulation stays on the clients because Starfield has no headless mode. See [docs/RESEARCH.md §7](docs/RESEARCH.md#7-dedicated-server-feasibility).
- More detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 or newer (or Build Tools) with the **Desktop development with C++** workload. This includes MSVC, CMake, Ninja and vcpkg.
- Internet access on the first configure (vcpkg dependencies and SFSE's `PluginAPI.h`)
- To run the plugin: Starfield **1.16.244** (Steam), [SFSE 0.2.21](https://github.com/ianpatt/sfse)

## Build and test

```powershell
# configure + build + run all tests (release preset)
./scripts/build.ps1

# debug build without tests
./scripts/build.ps1 -Preset debug -NoTest
```

`scripts/build.ps1` finds Visual Studio, loads the x64 developer environment, and uses `VCPKG_ROOT` (or the vcpkg bundled with Visual Studio). It then runs `cmake --preset`, `cmake --build --preset` and `ctest --preset`. The first configure builds OpenSSL, protobuf and GameNetworkingSockets, which takes a while.

Outputs (`build/release/`):
- `src/CoSyncSF.Server/cosync-server.exe`
- `src/CoSyncSF.Client/cosync-testclient.exe`
- `stage/Data/SFSE/Plugins/CoSyncSF.dll` + `CoSyncSF.ini` (copy `stage/Data` into the Starfield folder)

## Run a LAN session (no game needed)

```powershell
# terminal 1: session server on UDP 37015, discovery on 37016
build/release/src/CoSyncSF.Server/cosync-server.exe --name "Test Session" --max-players 4

# terminal 2 and 3: headless clients that send synthetic movement
build/release/src/CoSyncSF.Client/cosync-testclient.exe --connect 127.0.0.1:37015 --name Alice --send-transforms
build/release/src/CoSyncSF.Client/cosync-testclient.exe --connect 127.0.0.1:37015 --name Bob --send-transforms --log-level debug

# find sessions on the LAN
build/release/src/CoSyncSF.Client/cosync-testclient.exe --discover
```

From another PC, replace `127.0.0.1` with the server's LAN IP and allow UDP 37015/37016 through Windows Firewall. Pass `--require-runtime 1.16.244` to the server to admit only game clients on that build. Use `--password` or `COSYNC_PASSWORD` for a shared session password.

## Run the plugin (experimental)

1. Install SFSE 0.2.21 for Starfield 1.16.244.
2. Copy `build/release/stage/Data` into the Starfield install folder.
3. Edit `Data/SFSE/Plugins/CoSyncSF.ini`. Set `Mode=host` on one PC, or `Mode=client` with `Address=<host-ip>:37015`, and give each player a unique `Name`.
4. Launch through `sfse_loader.exe`. Check `Documents/My Games/Starfield/SFSE/Logs/CoSyncSF.log`.

At this milestone the plugin only connects and logs session events. It does not read or change game state.

## Known limitations

- LAN mode encrypts traffic (GNS) but does **not** authenticate peers cryptographically. Use it only on trusted networks; the password is a shared secret.
- Addresses must be numeric IPs (no DNS yet).
- The plugin supports exactly one runtime (1.16.244) and must be rebuilt for other game versions.
- On game exit the network thread is ended by the OS. The server notices through the heartbeat timeout (10 s) and keeps the slot for 30 s.
- No persistence: the session state lives only in the server process.

## Documentation

- [docs/RESEARCH.md](docs/RESEARCH.md): ecosystem findings, verified vs. unverified, licences
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): boundaries, threading, protocol, authority model
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones and status
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## License

Private project. Nothing is published or distributed until the project is complete. The project licence will be chosen at release. The SFSE plugin will link CommonLibSF (GPL-3.0+), so its source must be released under GPL-compatible terms when binaries are distributed. Third-party components keep their own licences; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
