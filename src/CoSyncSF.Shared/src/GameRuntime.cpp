#include "cosync/shared/GameRuntime.h"

#include <array>
#include <charconv>
#include <format>

namespace cosync::game
{
    std::string FormatRuntimeVersion(std::uint32_t v)
    {
        if (v == 0) {
            return "none";
        }
        return std::format("{}.{}.{}", RuntimeMajor(v), RuntimeMinor(v), RuntimeBuild(v));
    }

    std::optional<std::uint32_t> ParseRuntimeVersion(std::string_view text)
    {
        std::array<std::uint32_t, 3> parts{};
        const char* p = text.data();
        const char* end = text.data() + text.size();
        for (std::size_t i = 0; i < parts.size(); ++i) {
            auto [next, ec] = std::from_chars(p, end, parts[i]);
            if (ec != std::errc{} || next == p) {
                return std::nullopt;
            }
            p = next;
            if (i < 2) {
                if (p == end || *p != '.') return std::nullopt;
                ++p;
            }
        }
        if (p != end || parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFFF) {
            return std::nullopt;
        }
        return MakeRuntimeVersion(parts[0], parts[1], parts[2]);
    }
}
