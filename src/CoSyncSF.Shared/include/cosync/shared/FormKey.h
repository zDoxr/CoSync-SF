#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Load-order-independent identity for game forms.
//
// A Starfield FormID's high bits select the plugin that defines the form, and
// that selection depends on each player's load order. Two players with
// different mod lists can therefore have different FormIDs for the same cell.
// CoSync identifies forms by (plugin file name, local id) instead, and puts a
// 32-bit hash of that pair on the wire.
namespace cosync::forms
{
    enum class FileKind
    {
        Full,     // 0x00..0xFC: index = top byte, local = low 24 bits
        Medium,   // 0xFD: index = bits 16..23, local = low 16 bits
        Small,    // 0xFE: index = bits 12..23, local = low 12 bits
        Runtime,  // 0xFF: created at runtime, no plugin
    };

    struct FormIdParts
    {
        FileKind kind = FileKind::Full;
        std::uint32_t fileIndex = 0;  // index into the matching compiled file list
        std::uint32_t localId = 0;
    };

    [[nodiscard]] constexpr FormIdParts Decompose(std::uint32_t formId) noexcept
    {
        const std::uint32_t top = formId >> 24;
        switch (top) {
        case 0xFF: return { FileKind::Runtime, 0, formId };
        case 0xFE: return { FileKind::Small, (formId >> 12) & 0xFFF, formId & 0xFFF };
        case 0xFD: return { FileKind::Medium, (formId >> 16) & 0xFF, formId & 0xFFFF };
        default: return { FileKind::Full, top, formId & 0xFFFFFF };
        }
    }

    // 32-bit FNV-1a over the ASCII-lowercased file name, a ':' separator, and
    // the local id (little-endian). 0 is reserved for "unknown location".
    [[nodiscard]] std::uint32_t StableId(std::string_view fileName, std::uint32_t localId) noexcept;

    // "Starfield.esm:00012345" for logs.
    [[nodiscard]] std::string Describe(std::string_view fileName, std::uint32_t localId);
}
