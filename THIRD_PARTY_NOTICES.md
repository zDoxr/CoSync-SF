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
| CommonLibSF | commit 9d090c4c (RUNTIME_LATEST 1.16.244) | GPL-3.0-or-later + Modding/Linking exceptions | plugin (static) | https://github.com/libxse/commonlibsf |
| commonlib-shared | commit 9fbb74d6 | GPL-3.0-or-later + exceptions | plugin (static) | https://github.com/libxse/commonlib-shared |
| spdlog | vcpkg baseline (std::format, wchar) | MIT | plugin (CommonLibSF dependency) | https://github.com/gabime/spdlog |
| GoogleTest | 1.18.0 (vcpkg) | BSD-3-Clause | tests only, not distributed | https://github.com/google/googletest |

The exact dependency set is pinned by `builtin-baseline` in `vcpkg.json`.

Not included and not redistributed: Starfield (Bethesda), SFSE runtime binaries,
Address Library databases, Steamworks SDK.

Because the plugin links CommonLibSF, any future binary distribution of
CoSyncSF.dll must be accompanied by its source under GPL-3.0-compatible terms.
The project is private and nothing is distributed (see README).
