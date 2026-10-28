#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cosync
{
    static_assert(std::endian::native == std::endian::little, "wire format assumes a little-endian host");

    // Append-only little-endian writer. Strings are written with a u8 length prefix,
    // so every string field on the wire is limited to 255 bytes by construction.
    class ByteWriter
    {
    public:
        explicit ByteWriter(std::size_t reserve = 64) { _buf.reserve(reserve); }

        void U8(std::uint8_t v) { _buf.push_back(v); }
        void U16(std::uint16_t v) { Raw(&v, sizeof v); }
        void U32(std::uint32_t v) { Raw(&v, sizeof v); }
        void U64(std::uint64_t v) { Raw(&v, sizeof v); }
        void F32(float v) { Raw(&v, sizeof v); }
        void Bool(bool v) { U8(v ? 1 : 0); }

        void Bytes(std::span<const std::uint8_t> bytes) { Raw(bytes.data(), bytes.size()); }

        // Caller is responsible for validating length limits before encoding;
        // oversize strings are truncated defensively rather than corrupting the stream.
        void String(std::string_view s)
        {
            const auto len = static_cast<std::uint8_t>(s.size() > 255 ? 255 : s.size());
            U8(len);
            Raw(s.data(), len);
        }

        [[nodiscard]] const std::vector<std::uint8_t>& Data() const noexcept { return _buf; }
        [[nodiscard]] std::vector<std::uint8_t> Take() noexcept { return std::move(_buf); }

    private:
        void Raw(const void* p, std::size_t n)
        {
            const auto* b = static_cast<const std::uint8_t*>(p);
            _buf.insert(_buf.end(), b, b + n);
        }

        std::vector<std::uint8_t> _buf;
    };

    // Bounds-checked reader. Any read past the end sets a sticky failure flag and
    // yields zero values; callers check Ok() once after decoding a whole message.
    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const std::uint8_t> data) noexcept : _data(data) {}

        std::uint8_t U8() noexcept
        {
            std::uint8_t v = 0;
            Raw(&v, sizeof v);
            return v;
        }
        std::uint16_t U16() noexcept
        {
            std::uint16_t v = 0;
            Raw(&v, sizeof v);
            return v;
        }
        std::uint32_t U32() noexcept
        {
            std::uint32_t v = 0;
            Raw(&v, sizeof v);
            return v;
        }
        std::uint64_t U64() noexcept
        {
            std::uint64_t v = 0;
            Raw(&v, sizeof v);
            return v;
        }
        float F32() noexcept
        {
            float v = 0.0f;
            Raw(&v, sizeof v);
            return v;
        }

        // Only 0 and 1 are valid encodings; anything else is a malformed message.
        bool Bool() noexcept
        {
            const auto v = U8();
            if (v > 1) {
                _ok = false;
            }
            return v == 1;
        }

        void Bytes(std::span<std::uint8_t> out) noexcept { Raw(out.data(), out.size()); }

        // Reads a u8-length-prefixed string, failing if it exceeds maxBytes.
        std::string String(std::size_t maxBytes)
        {
            const std::size_t len = U8();
            if (!_ok) {
                return {};
            }
            if (len > Remaining()) {
                _ok = false;
                _overrun = true;
                return {};
            }
            if (len > maxBytes) {
                _ok = false;
                return {};
            }
            std::string s(reinterpret_cast<const char*>(_data.data() + _pos), len);
            _pos += len;
            return s;
        }

        void Fail() noexcept { _ok = false; }
        [[nodiscard]] bool Ok() const noexcept { return _ok; }
        // True if a read ran past the end of the buffer (vs. an invalid value).
        [[nodiscard]] bool Overrun() const noexcept { return _overrun; }
        [[nodiscard]] std::size_t Remaining() const noexcept { return _data.size() - _pos; }
        [[nodiscard]] bool AtEnd() const noexcept { return _pos == _data.size(); }

    private:
        void Raw(void* out, std::size_t n) noexcept
        {
            if (!_ok) {
                return;
            }
            if (n > Remaining()) {
                _ok = false;
                _overrun = true;
                return;
            }
            std::memcpy(out, _data.data() + _pos, n);
            _pos += n;
        }

        std::span<const std::uint8_t> _data;
        std::size_t _pos = 0;
        bool _ok = true;
        bool _overrun = false;
    };
}
