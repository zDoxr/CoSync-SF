# CoSync-SF — Research Findings

Last updated: 2026-10-27. Each finding is tagged:

- **[verified]** — confirmed from primary source code/docs (link given) or by running it here.
- **[reported]** — from secondary sources; not independently confirmed.
- **[unverified]** — plausible, but needs reverse engineering or an in-game test.

---

## 1. Target game runtime

| Item | Finding |
|---|---|
| Newest Starfield runtime | **1.16.244**, released 2026-06-11 (console build 1.000.006). [reported: patch-note coverage]. No newer public build was found; a September 2026 Steam beta is mentioned without a version. |
| SFSE support | SFSE **0.2.21** supports 1.16.244 (`CURRENT_RELEASE_RUNTIME = RUNTIME_VERSION_1_16_244 = 0x01100F40`). [verified: [sfse_common/sfse_version.h](https://github.com/ianpatt/sfse/blob/master/sfse_common/sfse_version.h), [sfse_whatsnew.txt](https://github.com/ianpatt/sfse/blob/master/sfse_whatsnew.txt)] |
| Version packing | `major<<24 \| minor<<16 \| build<<4` — mirrored in `GameRuntime.h` and unit-tested. [verified] |
| CommonLibSF support | `RUNTIME_LATEST = RUNTIME_SF_1_16_244`. [verified: [include/SFSE/Version.h](https://github.com/libxse/commonlibsf/blob/main/include/SFSE/Version.h)] |
| Local install | Starfield was **still downloading** on the dev machine during this pass (Steam manifest `SizeOnDisk 0`, `TargetBuildID 23518663`), so the installed `Starfield.exe` version has not been read yet. [verified locally] |
| Debug symbols | Bethesda does not publish PDBs for Starfield. Engine layouts come from community RE (CommonLibSF, Address Library). [reported] |

**Policy:** `kTargetRuntime` in `src/CoSyncSF.Shared/include/cosync/shared/GameRuntime.h` is the single place to change. Bump it only after reading the real `Starfield.exe` FileVersion and confirming SFSE (and CommonLibSF, once used) support it.

## 2. SFSE (Starfield Script Extender)

- Repo: [ianpatt/sfse](https://github.com/ianpatt/sfse). Active (last push 2026-06-11). [verified]
- Plugin interface: `SFSEPlugin_Version` (exported data) + `SFSEPlugin_Load` (+ optional `SFSEPlugin_Preload`). Interfaces: Messaging, Trampoline, Menu (Scaleform movie / manager callbacks), Task (`AddTask`, `AddTaskPermanent` — run on the **main thread**). [verified: [sfse/PluginAPI.h](https://github.com/ianpatt/sfse/blob/master/sfse/PluginAPI.h)]
- Messages: `PostLoad`, `PostPostLoad`, `PostDataLoad`, `PostPostDataLoad`, `Pre/PostSaveGame`, `Pre/PostLoadGame`. No "game exiting" message. [verified]
- Compatibility check ([sfse/PluginManager.cpp](https://github.com/ianpatt/sfse/blob/master/sfse/PluginManager.cpp)): a plugin is version-independent only if it declares (Signatures **or** AddressLibraryV2) **and** (NoStructs **or** 1_14_70 layout). Otherwise the running runtime must be in `compatibleVersions`. [verified]
  - CoSyncSF milestone 1 uses **no game addresses and no engine structures**. It declares `NoStructs` and pins `{1.16.244}`. That is truthful and needs a rebuild per game patch, which is intended until real version-independence exists.
- Licensing: the SFSE repo has **no blanket license** (GitHub reports NOASSERTION). Its LICENSE grants MIT terms **only to `sfse/PluginAPI.h`**. CoSync-SF therefore uses only that header, downloaded at a pinned commit with a SHA-256 check. [verified]
- 0.2.17 note: game 1.15.216 changed compiler settings and invalidated the old Address Library; plugins using it must declare Address Library v2. [verified: whatsnew]

## 3. CommonLibSF

- Repo: [libxse/commonlibsf](https://github.com/libxse/commonlibsf). Active (push 2026-10-06). Build: **xmake 3.0+**, C++23, MSVC or clang-cl. [verified: README]
- Provides reverse-engineered engine types: `RE::PlayerCharacter::GetSingleton()`, `RE::TESObjectREFR::GetPosition()/GetAngle()`, `RE::Actor`, plus Address Library ID tables (`IDs.h`). [verified: headers]
- **License: GPL-3.0-or-later** with a Modding Exception and a GPL-3.0 Linking Exception. If CoSync-SF links CommonLibSF and distributes binaries, **the plugin's source must be distributed under GPL-3.0-compatible terms**; Starfield itself and SFSE are exempt. [verified: [README](https://github.com/libxse/commonlibsf#license), [EXCEPTIONS](https://github.com/libxse/commonlibsf/blob/main/EXCEPTIONS)]
  - ⚠ **Decision needed from the project owner** before milestone 2 links it: accept GPL for the plugin (the server/network libraries can stay separately licensed if they don't link CommonLibSF), or avoid CommonLibSF and maintain our own minimal RE layer (more work, more risk).
- Build-system implication: CommonLibSF is xmake-first; CoSync-SF is CMake + vcpkg. Options are (a) build the plugin with xmake and keep the rest in CMake, (b) consume CommonLibSF as a prebuilt static lib, or (c) check whether a CMake/vcpkg port exists at integration time. [unverified which is cleanest]

## 4. Address Library

- Nexus mod 3256 ("Address Library for SFSE Plugins") maps stable IDs to per-version addresses. SFSE checks for it when a plugin declares Address Library usage. [verified: PluginManager.cpp; reported: Nexus page]
- Whether a 1.16.244 database is published has **not** been confirmed from a primary source; CommonLibSF's ID tables suggest it is. [unverified]
- Redistribution: Address Library databases are distributed by their author on Nexus; CoSync-SF must not bundle them, and users install them separately. [reported]

## 5. GameNetworkingSockets (GNS)

- Repo: [ValveSoftware/GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets), **BSD-3-Clause**, active (push 2026-10-03). [verified]
- Integrated through the vcpkg port **gamenetworkingsockets 1.6.1** (OpenSSL crypto, protobuf), static triplet `x64-windows-static-md`, with the `ice` feature **disabled** because it isn't needed for direct IP. [verified: [vcpkg port](https://github.com/microsoft/vcpkg/tree/master/ports/gamenetworkingsockets)]
- Facts used by the implementation, checked against the v1.6.1 headers [verified]:
  - Reliable and unreliable messages, fragmentation, encryption, per-connection status callbacks.
  - `k_ESteamNetworkingConfig_ConnectionUserData` set on a listen socket **is inherited** by accepted connections.
  - `RecvMaxMessageSize`, `RecvBufferSize` and `RecvBufferMessages` bound memory a peer can pin. A larger message closes the connection; covered by the `OversizeMessageIsRefusedByTransport` test.
  - `RunCallbacks()` is process-wide, so the transport queues callback copies and applies them on its owner thread.
- **No LAN discovery** exists in GNS. CoSync-SF adds a minimal UDP query/response (`LanDiscovery`): queries are padded to the response size so it cannot amplify traffic, and replies are rate-limited. Broadcast only reaches the local subnet and can be blocked by firewalls. Direct `IP:port` entry is always available. [verified by test on loopback; broadcast across a real LAN not yet tested]
- **Without a CA, GNS encrypts but does not authenticate peers.** Identity in LAN mode is therefore session-level: an optional shared password plus a server-issued 128-bit resume token. LAN mode is for trusted networks.

## 6. Steam connectivity

Primary source: [Steam Datagram Relay docs](https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay).

| Capability | Standalone GNS (what we build now) | Steamworks `ISteamNetworkingSockets` |
|---|---|---|
| Direct IP / LAN | ✅ | ✅ |
| Steam Datagram Relay | ❌ "does not support accessing the relay network" [verified: Valve docs] | ✅ via `CreateListenSocketP2P` / `ConnectP2P` |
| P2P NAT traversal | Needs your own signaling service + STUN/TURN (`ISteamNetworkingConnectionSignaling`) | Steam provides signaling |
| Authenticated identity | Only with your own certificate authority | Valve PKI certificates |

Plan for **Steam mode** (all [unverified] until tested in-game):

1. Starfield's Steam build loads `steam_api64.dll` and initializes Steam with its own AppID (1716740). An in-game plugin could use the already-initialized Steam client interfaces (`SteamNetworkingSockets()`, `SteamUser()`, `SteamFriends()`) through the flat API instead of shipping any Steam binaries.
2. Implement `SteamTransport : ITransport` using `ConnectP2P`/`CreateListenSocketP2P` with `SteamNetworkingIdentity`. Because the API shape matches GNS, the gameplay protocol needs no changes.
3. Identity: use the transport-authenticated `m_identityRemote` of a P2P connection and/or `ISteamUser::GetAuthTicketForWebApi`/`BeginAuthSession`. **Never trust a Steam ID sent inside a CoSync message.**
4. Invites and discovery would use Steam rich presence/lobbies (`ISteamMatchmaking`). That depends on Starfield's AppID configuration, which a mod cannot change.
5. Open questions and risks:
   - Whether SDR/P2P is usable under Starfield's AppID by a third-party mod.
   - Whether this is acceptable under Steam's terms.
   - The Steamworks SDK license: headers are needed to compile, and SDK files must not be redistributed.
   - Game Pass/Microsoft Store copies have no Steam API, so Steam mode would not exist there.
   - A standalone dedicated server cannot use Starfield's AppID for Steam game-server auth, so Steam identity for players of a dedicated server would need web-API ticket validation.

## 7. Dedicated-server feasibility

| Model | Description | Feasibility now |
|---|---|---|
| 1. Authoritative headless simulation | Server runs Starfield's simulation with no rendering or input | **Not feasible today.** No server binary exists. `Starfield.exe` is a D3D12 client with no headless mode, and SFSE loads only into the game process. Running the real game "headless" would need extensive RE of renderer/input/window initialization. [reported/unverified] |
| 2. Host-player | One player's game is the authority for world/NPC state | **Feasible path for gameplay.** Requires game hooks (Phase B+). |
| 3. Session server | Separate process for connections, identity, relay, persistence; clients simulate | **Implemented (prototype)**: `cosync-server`. |

Possible future hybrid: a dedicated machine runs a real, minimised game client as the world authority (model 2 without a human). That needs a GPU and a game licence per instance and is experimental. **CoSync-SF does not claim `cosync-server` simulates Starfield.**

## 8. Game-state synchronization: what it really takes

Sending positions is the smallest part. Each system needs engine access on the **main thread** and an authority rule:

| System | Engine surface (CommonLibSF names) | Status |
|---|---|---|
| Local transform | `PlayerCharacter::GetSingleton()`, `TESObjectREFR::GetPosition/GetAngle`, parent cell / worldspace | Read API exists in CommonLibSF [verified headers]; not called yet |
| Remote player proxy | Spawn an actor (PlaceAtMe-style), disable its AI/packages, set position each frame, drive animation graph variables | [unverified] needs RE and in-game testing |
| Animation | Behaviour graph variables / animation events | [unverified] |
| Location identity | FormIDs depend on load order; need (plugin filename, local FormID) pairs, plus planet/system context | Protocol carries an opaque `locationId` for now |
| Loading screens / cell transitions | Main thread stalls; remote updates must be coalesced (we use a latest-value mailbox) | Network side done; game side [unverified] |
| Spaceflight, landing, ship interiors | Separate world spaces and ship-as-reference semantics | Research |
| Inventory, combat, NPC AI, quests | Need hooks, ownership/authority model and validation | Research |
| Save games | Each client has its own save; shared progression needs a separate persistence model | Research |

## 9. Existing multiplayer efforts

- **Starfield Together** (from members of the Skyrim Together team) was abandoned in Dec 2023. The developer reported having ported roughly 70% of Skyrim Together's code, and said the code was released as open source. [reported: [Starfield Portal](https://starfieldportal.com/article/skyrim-together-modders-cease-work-on-starfield-together), [PC Gamer](https://www.pcgamer.com/creator-behind-hugely-popular-skyrim-co-op-mod-gives-up-on-the-starfield-version-of-it-because-drum-roll-please-this-game-is-fing-trash)]. The repository location could not be found during this pass. **Do not reuse its code without first verifying its license** (Skyrim Together Reborn is GPL-3.0).
- No working Starfield multiplayer mod was found as of 2026. [reported]

## 10. Dependencies and licences

| Dependency | Use | Licence | Distributed with plugin/server? |
|---|---|---|---|
| GameNetworkingSockets 1.6.1 | Transport | BSD-3-Clause | Yes (statically linked) |
| OpenSSL 3.x | GNS crypto | Apache-2.0 | Yes (static) |
| protobuf (+ abseil, utf8_range) | GNS internals | BSD-3 / Apache-2.0 / MIT | Yes (static) |
| SFSE `PluginAPI.h` | Plugin ABI | MIT (that file only) | Header only |
| GoogleTest | Tests | BSD-3-Clause | No |
| CommonLibSF (future) | Engine RE | GPL-3.0+ with exceptions | **Decision pending** |

See `THIRD_PARTY_NOTICES.md`. The CoSync-SF project licence itself is **not yet chosen** and is the owner's decision.
