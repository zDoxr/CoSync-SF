#pragma once

#include <array>
#include <cstdint>

namespace cosync
{
    // Server-assigned, never reused within a session. 0 is never a valid player.
    using PlayerId = std::uint32_t;
    inline constexpr PlayerId kInvalidPlayerId = 0;

    // Random per server run; lets clients detect that a server restarted.
    using SessionId = std::uint64_t;

    // Opaque server-issued secret used to reclaim a player slot after a dropped
    // connection. Compared in constant time; all-zero means "no token".
    struct ResumeToken
    {
        std::array<std::uint8_t, 16> bytes{};

        [[nodiscard]] bool IsZero() const noexcept
        {
            std::uint8_t acc = 0;
            for (auto b : bytes) {
                acc |= b;
            }
            return acc == 0;
        }

        [[nodiscard]] bool ConstantTimeEquals(const ResumeToken& other) const noexcept
        {
            std::uint8_t diff = 0;
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                diff |= static_cast<std::uint8_t>(bytes[i] ^ other.bytes[i]);
            }
            return diff == 0;
        }
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };
}
