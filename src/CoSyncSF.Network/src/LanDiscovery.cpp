#include "cosync/net/LanDiscovery.h"

#include "cosync/shared/ByteStream.h"
#include "cosync/shared/Log.h"
#include "cosync/shared/Protocol.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <format>

namespace cosync::net::discovery
{
    namespace
    {
        constexpr SOCKET kInvalid = INVALID_SOCKET;

        bool StartWsa(std::string& error)
        {
            WSADATA data{};
            if (const int rc = WSAStartup(MAKEWORD(2, 2), &data); rc != 0) {
                error = std::format("WSAStartup failed ({})", rc);
                return false;
            }
            return true;
        }

        SOCKET OpenUdp(std::string& error)
        {
            const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == kInvalid) {
                error = std::format("socket() failed ({})", WSAGetLastError());
                return kInvalid;
            }
            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            return s;
        }

        std::string AddressString(const sockaddr_in& addr)
        {
            char buf[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf);
            return buf;
        }
    }

    std::vector<std::uint8_t> EncodeQuery(std::uint32_t nonce)
    {
        ByteWriter w(kPacketBytes);
        w.U32(kQueryMagic);
        w.U16(proto::kProtocolMajor);
        w.U32(nonce);
        auto bytes = w.Take();
        bytes.resize(kPacketBytes, 0);  // anti-amplification padding
        return bytes;
    }

    std::optional<std::uint32_t> DecodeQuery(std::span<const std::uint8_t> packet)
    {
        if (packet.size() != kPacketBytes) {
            return std::nullopt;
        }
        ByteReader r(packet);
        const auto magic = r.U32();
        r.U16();  // querier's protocol major; informational
        const auto nonce = r.U32();
        if (!r.Ok() || magic != kQueryMagic) {
            return std::nullopt;
        }
        return nonce;
    }

    std::vector<std::uint8_t> EncodeResponse(std::uint32_t nonce, const ServerInfo& info)
    {
        ByteWriter w(kPacketBytes);
        w.U32(kResponseMagic);
        w.U32(nonce);
        w.U16(info.protocolMajor);
        w.U16(info.protocolMinor);
        w.U16(info.gamePort);
        w.U16(info.players);
        w.U16(info.maxPlayers);
        w.U32(info.requiredGameRuntime);
        w.Bool(info.passwordRequired);
        w.String(info.serverName.substr(0, proto::kMaxServerNameBytes));
        return w.Take();  // 25 + 1 + 64 bytes max, always <= kPacketBytes
    }

    std::optional<std::pair<std::uint32_t, ServerInfo>> DecodeResponse(std::span<const std::uint8_t> packet)
    {
        if (packet.size() > kPacketBytes) {
            return std::nullopt;
        }
        ByteReader r(packet);
        ServerInfo info;
        const auto magic = r.U32();
        const auto nonce = r.U32();
        info.protocolMajor = r.U16();
        info.protocolMinor = r.U16();
        info.gamePort = r.U16();
        info.players = r.U16();
        info.maxPlayers = r.U16();
        info.requiredGameRuntime = r.U32();
        info.passwordRequired = r.Bool();
        info.serverName = r.String(proto::kMaxServerNameBytes);
        if (!r.Ok() || !r.AtEnd() || magic != kResponseMagic || info.gamePort == 0 ||
            !proto::IsValidText(info.serverName, proto::kMaxServerNameBytes)) {
            return std::nullopt;
        }
        return std::pair{ nonce, std::move(info) };
    }

    Responder::~Responder() { Stop(); }

    bool Responder::Start(std::uint16_t port, std::string& error)
    {
        Stop();
        if (!StartWsa(error)) {
            return false;
        }
        _wsa = true;
        const SOCKET s = OpenUdp(error);
        if (s == kInvalid) {
            Stop();
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            error = std::format("could not bind discovery UDP port {} ({})", port, WSAGetLastError());
            closesocket(s);
            Stop();
            return false;
        }
        _socket = static_cast<std::uintptr_t>(s);
        _limit = TokenBucket(20.0, 40.0, SteadyNowMs());
        log::Info("LAN discovery responder on UDP port {}", port);
        return true;
    }

    void Responder::Poll(const ServerInfo& info)
    {
        const auto s = static_cast<SOCKET>(_socket);
        if (s == kInvalid) {
            return;
        }
        std::uint8_t buf[kPacketBytes + 1];
        for (int i = 0; i < 64; ++i) {  // bounded work per poll
            sockaddr_in from{};
            int fromLen = sizeof from;
            const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) {
                break;  // WSAEWOULDBLOCK or error (e.g. ICMP port unreachable on Windows)
            }
            const auto nonce = DecodeQuery({ buf, static_cast<std::size_t>(n) });
            if (!nonce || !_limit.TryTake(SteadyNowMs())) {
                continue;
            }
            const auto reply = EncodeResponse(*nonce, info);
            sendto(s, reinterpret_cast<const char*>(reply.data()), static_cast<int>(reply.size()), 0,
                   reinterpret_cast<const sockaddr*>(&from), fromLen);
        }
    }

    void Responder::Stop()
    {
        if (static_cast<SOCKET>(_socket) != kInvalid) {
            closesocket(static_cast<SOCKET>(_socket));
            _socket = static_cast<std::uintptr_t>(kInvalid);
        }
        if (_wsa) {
            WSACleanup();
            _wsa = false;
        }
    }

    std::vector<DiscoveredServer> Discover(std::uint16_t port, std::uint32_t timeoutMs, bool broadcast,
                                           std::span<const std::string> unicastHosts, std::string& error)
    {
        std::vector<DiscoveredServer> found;
        if (!StartWsa(error)) {
            return found;
        }
        const SOCKET s = OpenUdp(error);
        if (s == kInvalid) {
            WSACleanup();
            return found;
        }

        std::uint32_t nonce = 0;
        if (!FillRandom({ reinterpret_cast<std::uint8_t*>(&nonce), sizeof nonce })) {
            nonce = static_cast<std::uint32_t>(SteadyNowMs());
        }
        const auto query = EncodeQuery(nonce);
        const auto start = SteadyNowMs();

        auto sendTo = [&](in_addr target) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(port);
            to.sin_addr = target;
            sendto(s, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof to);
        };

        if (broadcast) {
            BOOL on = TRUE;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
            in_addr bcast{};
            bcast.s_addr = htonl(INADDR_BROADCAST);
            sendTo(bcast);
        }
        for (const auto& host : unicastHosts) {
            in_addr target{};
            if (inet_pton(AF_INET, host.c_str(), &target) == 1) {
                sendTo(target);
            } else {
                log::Warn("discovery: '{}' is not a numeric IPv4 address", host);
            }
        }

        while (SteadyNowMs() - start < timeoutMs) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(s, &readSet);
            timeval tv{ 0, 50 * 1000 };
            if (select(0, &readSet, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            std::uint8_t buf[kPacketBytes + 1];
            sockaddr_in from{};
            int fromLen = sizeof from;
            const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) {
                continue;
            }
            auto decoded = DecodeResponse({ buf, static_cast<std::size_t>(n) });
            if (!decoded || decoded->first != nonce) {
                continue;
            }
            DiscoveredServer server;
            server.endpoint = Endpoint{ AddressString(from), decoded->second.gamePort };
            server.info = std::move(decoded->second);
            server.latencyMs = static_cast<std::uint32_t>(SteadyNowMs() - start);
            const bool duplicate = std::ranges::any_of(found, [&](const DiscoveredServer& d) {
                return d.endpoint.host == server.endpoint.host && d.endpoint.port == server.endpoint.port;
            });
            if (!duplicate) {
                found.push_back(std::move(server));
            }
        }

        closesocket(s);
        WSACleanup();
        return found;
    }
}
