# Third-Party Notices

CoSync-SF binaries statically link, or are built against, the components below.
Each component remains under its own licence. Full licence texts are installed
by vcpkg under `build/vcpkg_installed/x64-windows-static-md/share/<port>/copyright`
and must accompany any binary distribution.

| Component | Version | Licence | Used by | Source |
|---|---|---|---|---|
| GameNetworkingSockets | 1.6.1 (vcpkg) | BSD-3-Clause | network, server, plugin | https://github.com/ValveSoftware/GameNetworkingSockets |
| OpenSSL | vcpkg baseline | Apache-2.0 | GNS crypto | https://www.openssl.org/ |
| Protocol Buffers | vcpkg baseline | BSD-3-Clause | GNS internals | https://github.com/protocolbuffers/protobuf |
| Abseil | vcpkg baseline | Apache-2.0 | protobuf dependency | https://github.com/abseil/abseil-cpp |
| utf8_range | vcpkg baseline | MIT | protobuf dependency | https://github.com/protocolbuffers/utf8_range |
| SFSE `sfse/PluginAPI.h` | commit 48535cc4 (SFSE 0.2.21) | MIT (that file only) | plugin (header) | https://github.com/ianpatt/sfse |
| GoogleTest | 1.18.0 (vcpkg) | BSD-3-Clause | tests only, not distributed | https://github.com/google/googletest |

The exact dependency set is pinned by `builtin-baseline` in `vcpkg.json`.

Not included and not redistributed: Starfield (Bethesda), SFSE runtime binaries,
Address Library databases, Steamworks SDK.
