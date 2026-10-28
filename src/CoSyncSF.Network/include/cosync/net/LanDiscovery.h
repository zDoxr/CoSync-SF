#pragma once

#include "cosync/net/Transport.h"
#include "cosync/net/Util.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// LAN session discovery over plain UDP. GameNetworkingSockets has no LAN
// discovery of its own, so CoSync-SF answers small query datagrams on a
// separate port. Broadcast only reaches the local subnet and is subject to
// firewalls; direct address entry always remains available.
namespace cosync::net::discovery
{
    inline constexpr std::uint16_t kDefaultPort = 37016;
    inline constexpr std::uint32_t kQueryMagic = 0x51445343;     // "CSDQ"
    inline constexpr std::uint32_t kResponseMagic = 0x52445343;  // "CSDR"
    // Queries are padded to this size and responses never exceed it, so the
    // responder cannot be used for traffic amplification.
    inline constexpr std::size_t kPacketBytes = 128;

    struct ServerInfo
    {
        std::uint16_t protocolMajor = 0;
        std::uint16_t protocolMinor = 0;
        std::uint16_t gamePort = 0;
        std::uint16_t players = 0;
        std::uint16_t maxPlayers = 0;
        std::uint32_t requiredGameRuntime = 0;
        bool passwordRequired = false;
        std::string serverName;
    };

    struct DiscoveredServer
    {
        Endpoint endpoint;  // sender address + advertised game port
        ServerInfo info;
        std::uint32_t latencyMs = 0;
    };

    [[nodiscard]] std::vector<std::uint8_t> EncodeQuery(std::uint32_t nonce);
    [[nodiscard]] std::optional<std::uint32_t> DecodeQuery(std::span<const std::uint8_t> packet);
    [[nodiscard]] std::vector<std::uint8_t> EncodeResponse(std::uint32_t nonce, const ServerInfo& info);
    [[nodiscard]] std::optional<std::pair<std::uint32_t, ServerInfo>> DecodeResponse(std::span<const std::uint8_t> packet);

    // Answers discovery queries. Non-blocking; call Poll() from the server loop.
    class Responder
    {
    public:
        Responder() = default;
        ~Responder();
        Responder(const Responder&) = delete;
        Responder& operator=(const Responder&) = delete;

        bool Start(std::uint16_t port, std::string& error);
        void Poll(const ServerInfo& info);
        void Stop();

    private:
        std::uintptr_t _socket = ~std::uintptr_t{ 0 };
        bool _wsa = false;
        TokenBucket _limit;
    };

    // Sends a query to the subnet broadcast address (if enabled) and to each of
    // `unicastHosts`, then collects responses for `timeoutMs`. Blocking.
    std::vector<DiscoveredServer> Discover(std::uint16_t port, std::uint32_t timeoutMs, bool broadcast,
                                           std::span<const std::string> unicastHosts, std::string& error);
}
