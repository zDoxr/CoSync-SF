#include "cosync/net/GnsTransport.h"

#include "cosync/shared/Log.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <format>
#include <unordered_set>

namespace cosync::net
{
    namespace
    {
        // GameNetworkingSockets_Init/Kill are process-wide; reference-count them so
        // several transports (e.g. an in-process host server and client) can coexist.
        std::mutex g_libMutex;
        int g_libRefs = 0;

        // Live transports. Held while queueing a callback so the target cannot be
        // destroyed concurrently, and so a stale userdata pointer is never used.
        std::mutex g_liveMutex;
        std::unordered_set<GnsTransport*> g_live;

        void DebugOutput(ESteamNetworkingSocketsDebugOutputType type, const char* msg)
        {
            if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error) {
                log::Error("[gns] {}", msg);
            } else if (type <= k_ESteamNetworkingSocketsDebugOutputType_Warning) {
                log::Warn("[gns] {}", msg);
            } else {
                log::Debug("[gns] {}", msg);
            }
        }

        bool AcquireLibrary(std::string& error)
        {
            std::scoped_lock lock(g_libMutex);
            if (g_libRefs == 0) {
                SteamNetworkingErrMsg err{};
                if (!GameNetworkingSockets_Init(nullptr, err)) {
                    error = std::format("GameNetworkingSockets_Init failed: {}", err);
                    return false;
                }
                SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, DebugOutput);
            }
            ++g_libRefs;
            return true;
        }

        void ReleaseLibrary()
        {
            std::scoped_lock lock(g_libMutex);
            if (--g_libRefs == 0) {
                GameNetworkingSockets_Kill();
            }
        }

        void StatusTrampoline(SteamNetConnectionStatusChangedCallback_t* info)
        {
            auto* self = reinterpret_cast<GnsTransport*>(static_cast<std::intptr_t>(info->m_info.m_nUserData));
            std::scoped_lock lock(g_liveMutex);
            if (g_live.contains(self)) {
                self->QueueStatusChange({ info->m_hConn, info->m_info.m_eState, info->m_info.m_hListenSocket,
                                          info->m_info.m_eEndReason, info->m_info.m_szEndDebug });
            } else if (info->m_info.m_eState != k_ESteamNetworkingConnectionState_None) {
                SteamNetworkingSockets()->CloseConnection(info->m_hConn, 0, "no owner", false);
            }
        }

        std::vector<SteamNetworkingConfigValue_t> MakeOptions(const GnsTransport* self, const GnsOptions& o)
        {
            std::vector<SteamNetworkingConfigValue_t> opts(7);
            opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                           reinterpret_cast<void*>(&StatusTrampoline));
            // Inherited by connections accepted on a listen socket (documented GNS behaviour).
            opts[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                             static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(self)));
            opts[2].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, static_cast<std::int32_t>(o.maxMessageBytes));
            opts[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, o.recvBufferBytes);
            opts[4].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, o.recvBufferMessages);
            opts[5].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, o.connectTimeoutMs);
            opts[6].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, o.idleTimeoutMs);
            return opts;
        }

        HSteamNetConnection ToHandle(ConnectionId id) { return static_cast<HSteamNetConnection>(id); }

        // GNS binds its UDP socket in a way that lets a second listener (in this
        // or another process) bind the same port without error, silently
        // splitting traffic. Probe with exclusive binds first so a port that is
        // already in use is reported as an error. (Small race window remains.)
        bool UdpPortInUse(std::uint16_t port)
        {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                return false;  // cannot probe; let GNS decide
            }
            bool inUse = false;
            for (const int family : { AF_INET6, AF_INET }) {
                const SOCKET s = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
                if (s == INVALID_SOCKET) {
                    continue;
                }
                BOOL exclusive = TRUE;
                setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
                int rc = 0;
                if (family == AF_INET6) {
                    DWORD v6only = 0;  // dual-stack, like GNS
                    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
                    sockaddr_in6 addr{};
                    addr.sin6_family = AF_INET6;
                    addr.sin6_port = htons(port);
                    addr.sin6_addr = in6addr_any;
                    rc = ::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
                } else {
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(port);
                    addr.sin_addr.s_addr = htonl(INADDR_ANY);
                    rc = ::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
                }
                const int err = rc == 0 ? 0 : WSAGetLastError();
                closesocket(s);
                if (err == WSAEADDRINUSE || err == WSAEACCES) {
                    inUse = true;
                    break;
                }
            }
            WSACleanup();
            return inUse;
        }
    }

    GnsTransport::GnsTransport(GnsOptions options) : _options(options)
    {
        if (!AcquireLibrary(_initError)) {
            log::Error("{}", _initError);
            return;
        }
        _pollGroup = SteamNetworkingSockets()->CreatePollGroup();
        {
            std::scoped_lock lock(g_liveMutex);
            g_live.insert(this);
        }
        _ready = true;
    }

    GnsTransport::~GnsTransport()
    {
        if (!_ready) {
            return;
        }
        Shutdown();
        {
            std::scoped_lock lock(g_liveMutex);
            g_live.erase(this);
        }
        SteamNetworkingSockets()->DestroyPollGroup(_pollGroup);
        ReleaseLibrary();
    }

    bool GnsTransport::Listen(std::uint16_t port, std::string& error)
    {
        if (!_ready) {
            error = _initError;
            return false;
        }
        if (_listenSocket != k_HSteamListenSocket_Invalid) {
            error = "already listening";
            return false;
        }
        if (UdpPortInUse(port)) {
            error = std::format("UDP port {} is already in use (another host or server is running)", port);
            return false;
        }
        SteamNetworkingIPAddr addr;
        addr.Clear();  // [::] dual-stack, accepts IPv4 as well
        addr.m_port = port;
        const auto opts = MakeOptions(this, _options);
        _listenSocket = SteamNetworkingSockets()->CreateListenSocketIP(addr, static_cast<int>(opts.size()), opts.data());
        if (_listenSocket == k_HSteamListenSocket_Invalid) {
            error = std::format("could not listen on UDP port {}", port);
            return false;
        }
        return true;
    }

    ConnectionId GnsTransport::Connect(const Endpoint& endpoint, std::string& error)
    {
        if (!_ready) {
            error = _initError;
            return kInvalidConnection;
        }
        SteamNetworkingIPAddr addr;
        addr.Clear();
        if (!addr.ParseString(endpoint.ToString().c_str())) {
            error = std::format("'{}' is not a numeric IP address and port", endpoint.ToString());
            return kInvalidConnection;
        }
        const auto opts = MakeOptions(this, _options);
        const auto conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, static_cast<int>(opts.size()), opts.data());
        if (conn == k_HSteamNetConnection_Invalid) {
            error = std::format("could not start connection to {}", endpoint.ToString());
            return kInvalidConnection;
        }
        SteamNetworkingSockets()->SetConnectionPollGroup(conn, _pollGroup);
        _connections[conn] = ConnState{ true, false };
        return conn;
    }

    bool GnsTransport::Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery)
    {
        const auto conn = ToHandle(connection);
        if (!_connections.contains(conn)) {
            return false;
        }
        const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
        const auto result = SteamNetworkingSockets()->SendMessageToConnection(
            conn, data.data(), static_cast<std::uint32_t>(data.size()), flags, nullptr);
        return result == k_EResultOK;
    }

    void GnsTransport::Close(ConnectionId connection, CloseCode code, std::string_view reason, bool linger)
    {
        const auto conn = ToHandle(connection);
        if (_connections.erase(conn) == 0) {
            return;
        }
        const std::string text(reason);
        SteamNetworkingSockets()->CloseConnection(conn, static_cast<int>(code), text.c_str(), linger);
        _pending.push_back({ TransportEventType::Disconnected, connection, {}, static_cast<int>(code), text, true });
    }

    void GnsTransport::QueueStatusChange(StatusChange change)
    {
        std::scoped_lock lock(_statusMutex);
        _statusQueue.push_back(std::move(change));
    }

    void GnsTransport::ApplyStatusChange(const StatusChange& change)
    {
        auto* sockets = SteamNetworkingSockets();
        const auto conn = change.connection;

        switch (change.state) {
        case k_ESteamNetworkingConnectionState_Connecting:
            if (change.listenSocket != k_HSteamListenSocket_Invalid) {
                if (change.listenSocket != _listenSocket) {
                    sockets->CloseConnection(conn, static_cast<int>(CloseCode::ServerShutdown), "not listening", false);
                    break;
                }
                if (sockets->AcceptConnection(conn) != k_EResultOK) {
                    sockets->CloseConnection(conn, 0, "accept failed", false);
                    break;
                }
                sockets->SetConnectionPollGroup(conn, _pollGroup);
                _connections[conn] = ConnState{ false, false };
            }
            break;

        case k_ESteamNetworkingConnectionState_Connected:
            if (auto it = _connections.find(conn); it != _connections.end() && !it->second.announced) {
                it->second.announced = true;
                _pending.push_back({ TransportEventType::Connected, conn });
            }
            break;

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            const auto it = _connections.find(conn);
            if (it == _connections.end()) {
                break;  // already closed locally, or never accepted
            }
            if (it->second.announced || it->second.outgoing) {
                // Deliver anything the peer sent before closing (e.g. a Reject
                // flushed with linger) ahead of the Disconnected event.
                DrainConnection(conn);
                _pending.push_back({ TransportEventType::Disconnected, conn, {}, change.endReason, change.endDebug, false });
            }
            _connections.erase(it);
            sockets->CloseConnection(conn, 0, nullptr, false);
            break;
        }

        default:
            break;
        }
    }

    void GnsTransport::DrainConnection(std::uint32_t connection)
    {
        SteamNetworkingMessage_t* msgs[32];
        int n = 0;
        while ((n = SteamNetworkingSockets()->ReceiveMessagesOnConnection(connection, msgs, 32)) > 0) {
            for (int i = 0; i < n; ++i) {
                const auto size = static_cast<std::size_t>(msgs[i]->m_cbSize);
                if (size <= _options.maxMessageBytes) {
                    const auto* p = static_cast<const std::uint8_t*>(msgs[i]->m_pData);
                    _pending.push_back({ TransportEventType::Message, connection, { p, p + size } });
                }
                msgs[i]->Release();
            }
        }
    }

    void GnsTransport::Poll(std::vector<TransportEvent>& out)
    {
        if (!_ready) {
            return;
        }
        auto* sockets = SteamNetworkingSockets();
        sockets->RunCallbacks();

        std::vector<StatusChange> changes;
        {
            std::scoped_lock lock(_statusMutex);
            changes.swap(_statusQueue);
        }
        for (const auto& change : changes) {
            ApplyStatusChange(change);
        }

        SteamNetworkingMessage_t* msgs[64];
        int n = 0;
        while ((n = sockets->ReceiveMessagesOnPollGroup(_pollGroup, msgs, 64)) > 0) {
            for (int i = 0; i < n; ++i) {
                auto* m = msgs[i];
                const auto it = _connections.find(m->m_conn);
                if (it != _connections.end()) {
                    // The Connected callback can be queued after RunCallbacks() returned
                    // while the peer's first message is already here. Data only flows on
                    // a connected connection, so announce it now to keep event order.
                    if (!it->second.announced) {
                        it->second.announced = true;
                        _pending.push_back({ TransportEventType::Connected, m->m_conn });
                    }
                    const auto size = static_cast<std::size_t>(m->m_cbSize);
                    if (size > _options.maxMessageBytes) {
                        Close(m->m_conn, CloseCode::Oversize, "message too large", false);
                    } else {
                        const auto* p = static_cast<const std::uint8_t*>(m->m_pData);
                        _pending.push_back({ TransportEventType::Message, m->m_conn, { p, p + size } });
                    }
                }
                m->Release();
            }
        }

        for (auto& ev : _pending) {
            out.push_back(std::move(ev));
        }
        _pending.clear();
    }

    void GnsTransport::Shutdown()
    {
        if (!_ready) {
            return;
        }
        auto* sockets = SteamNetworkingSockets();
        for (const auto& [conn, state] : _connections) {
            sockets->CloseConnection(conn, static_cast<int>(CloseCode::ServerShutdown), "shutdown", true);
        }
        _connections.clear();
        if (_listenSocket != k_HSteamListenSocket_Invalid) {
            sockets->CloseListenSocket(_listenSocket);
            _listenSocket = k_HSteamListenSocket_Invalid;
        }
    }

    std::optional<ConnectionStats> GnsTransport::GetStats(ConnectionId connection)
    {
        const auto conn = ToHandle(connection);
        if (!_connections.contains(conn)) {
            return std::nullopt;
        }
        SteamNetConnectionRealTimeStatus_t status{};
        if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(conn, &status, 0, nullptr) != k_EResultOK) {
            return std::nullopt;
        }
        return ConnectionStats{ status.m_nPing, status.m_flConnectionQualityLocal, status.m_cbPendingReliable };
    }

    std::string GnsTransport::Describe(ConnectionId connection)
    {
        SteamNetConnectionInfo_t info{};
        if (!SteamNetworkingSockets()->GetConnectionInfo(ToHandle(connection), &info)) {
            return std::format("gns#{}", connection);
        }
        return info.m_szConnectionDescription;
    }
}
