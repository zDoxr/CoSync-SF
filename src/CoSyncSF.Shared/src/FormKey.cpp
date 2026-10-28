#include "cosync/shared/FormKey.h"

#include <format>

namespace cosync::forms
{
    std::uint32_t StableId(std::string_view fileName, std::uint32_t localId) noexcept
    {
        std::uint32_t hash = 2166136261u;
        auto mix = [&hash](std::uint8_t byte) {
            hash ^= byte;
            hash *= 16777619u;
        };
        for (const char c : fileName) {
            mix(static_cast<std::uint8_t>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
        }
        mix(':');
        for (int shift = 0; shift < 32; shift += 8) {
            mix(static_cast<std::uint8_t>(localId >> shift));
        }
        return hash == 0 ? 1 : hash;
    }

    std::string Describe(std::string_view fileName, std::uint32_t localId)
    {
        return std::format("{}:{:08X}", fileName, localId);
    }
}
