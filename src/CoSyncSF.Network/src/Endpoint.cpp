#include "cosync/net/Transport.h"

#include <charconv>
#include <format>

namespace cosync::net
{
    namespace
    {
        std::optional<std::uint16_t> ParsePort(std::string_view s)
        {
            unsigned value = 0;
            const auto* end = s.data() + s.size();
            auto [ptr, ec] = std::from_chars(s.data(), end, value);
            if (ec != std::errc{} || ptr != end || value == 0 || value > 65535) {
                return std::nullopt;
            }
            return static_cast<std::uint16_t>(value);
        }
    }

    std::optional<Endpoint> Endpoint::Parse(std::string_view text, std::uint16_t defaultPort)
    {
        if (text.empty()) {
            return std::nullopt;
        }

        Endpoint ep;
        // "steam:<SteamID64>": a Steam P2P peer (SteamTransport), virtual port 0.
        if (text.starts_with("steam:")) {
            const auto digits = text.substr(6);
            std::uint64_t id = 0;
            const auto* end = digits.data() + digits.size();
            auto [ptr, ec] = std::from_chars(digits.data(), end, id);
            if (ec != std::errc{} || ptr != end || id == 0) return std::nullopt;
            ep.host = std::string(text);
            ep.port = 0;
            return ep;
        }
        if (text.front() == '[') {
            // [ipv6]:port or [ipv6]
            const auto close = text.find(']');
            if (close == std::string_view::npos || close == 1) {
                return std::nullopt;
            }
            ep.host = std::string(text.substr(1, close - 1));
            const auto rest = text.substr(close + 1);
            if (rest.empty()) {
                ep.port = defaultPort;
            } else if (rest.front() == ':') {
                auto port = ParsePort(rest.substr(1));
                if (!port) return std::nullopt;
                ep.port = *port;
            } else {
                return std::nullopt;
            }
        } else {
            const auto colon = text.rfind(':');
            if (colon != std::string_view::npos && text.find(':') != colon) {
                // Multiple colons without brackets: bare IPv6 address, no port.
                ep.host = std::string(text);
                ep.port = defaultPort;
            } else if (colon != std::string_view::npos) {
                ep.host = std::string(text.substr(0, colon));
                auto port = ParsePort(text.substr(colon + 1));
                if (!port || ep.host.empty()) return std::nullopt;
                ep.port = *port;
            } else {
                ep.host = std::string(text);
                ep.port = defaultPort;
            }
        }

        if (ep.port == 0) {
            return std::nullopt;
        }
        return ep;
    }

    std::string Endpoint::ToString() const
    {
        if (host.starts_with("steam:")) return host;  // Steam P2P peer
        if (host.find(':') != std::string::npos) {
            return std::format("[{}]:{}", host, port);
        }
        return std::format("{}:{}", host, port);
    }
}
