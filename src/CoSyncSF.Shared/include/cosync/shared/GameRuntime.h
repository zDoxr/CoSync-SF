#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cosync::game
{
    // Same packing SFSE uses for RUNTIME_VERSION_* (sfse_common/sfse_version.h):
    // major << 24 | minor << 16 | build << 4.
    constexpr std::uint32_t MakeRuntimeVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t build) noexcept
    {
        return ((major & 0xFF) << 24) | ((minor & 0xFF) << 16) | ((build & 0xFFF) << 4);
    }

    constexpr std::uint32_t RuntimeMajor(std::uint32_t v) noexcept { return (v >> 24) & 0xFF; }
    constexpr std::uint32_t RuntimeMinor(std::uint32_t v) noexcept { return (v >> 16) & 0xFF; }
    constexpr std::uint32_t RuntimeBuild(std::uint32_t v) noexcept { return (v >> 4) & 0xFFF; }

    // Newest Starfield runtime this project targets. 1.16.244 (released 2026-06-11)
    // is the newest version supported by SFSE (v0.2.21, CURRENT_RELEASE_RUNTIME).
    // Update only after verifying a newer Starfield.exe and a matching SFSE release.
    inline constexpr std::uint32_t kTargetRuntime = MakeRuntimeVersion(1, 16, 244);
    static_assert(kTargetRuntime == 0x01100F40, "must match SFSE RUNTIME_VERSION_1_16_244");

    std::string FormatRuntimeVersion(std::uint32_t v);

    // Parses "1.16.244" into the packed form; nullopt if malformed or out of range.
    std::optional<std::uint32_t> ParseRuntimeVersion(std::string_view text);
}
