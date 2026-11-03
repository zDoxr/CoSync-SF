#include "cosync/net/SteamTransport.h"

#include "cosync/shared/Log.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <windows.h>

#include <charconv>
#include <format>
#include <unordered_set>

namespace cosync::net
{
    namespace
    {
        // Flat API of the game's steam_api64.dll (steam_api_flat.h signatures; references are passed
        // as pointers by the ABI).
        struct Flat
        {
            void* (*sockets)() = nullptr;
            void* (*utils)() = nullptr;
            void (*initRelay)(void*) = nullptr;
            HSteamListenSocket (*listenP2P)(void*, int, int, const SteamNetworkingConfigValue_t*) = nullptr;
            HSteamNetConnection (*connectP2P)(void*, const SteamNetworkingIdentity*, int, int,
                                              const SteamNetworkingConfigValue_t*) = nullptr;
            EResult (*accept)(void*, HSteamNetConnection) = nullptr;
            bool (*close)(void*, HSteamNetConnection, int, const char*, bool) = nullptr;
            bool (*closeListen)(void*, HSteamListenSocket) = nullptr;
            EResult (*send)(void*, HSteamNetConnection, const void*, std::uint32_t, int, std::int64_t*) = nullptr;
            int (*recvGroup)(void*, HSteamNetPollGroup, SteamNetworkingMessage_t**, int) = nullptr;
            int (*recvConn)(void*, HSteamNetConnection, SteamNetworkingMessage_t**, int) = nullptr;
            HSteamNetPollGroup (*createGroup)(void*) = nullptr;
            bool (*destroyGroup)(void*, HSteamNetPollGroup) = nullptr;
            bool (*setGroup)(void*, HSteamNetConnection, HSteamNetPollGroup) = nullptr;
            void (*runCallbacks)(void*) = nullptr;
            bool (*info)(void*, HSteamNetConnection, SteamNetConnectionInfo_t*) = nullptr;
            EResult (*realtime)(void*, HSteamNetConnection, SteamNetConnectionRealTimeStatus_t*, int,
                                SteamNetConnectionRealTimeLaneStatus_t*) = nullptr;
            bool complete = false;
        };

        const Flat& Api()
        {
            static const Flat flat = [] {
                Flat f;
                HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
                if (!dll) return f;
                auto get = [dll](auto& fn, const char* name) {
                    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
                    return fn != nullptr;
                };
                bool ok = get(f.sockets, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
                ok &= get(f.utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
                ok &= get(f.initRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
                ok &= get(f.listenP2P, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
                ok &= get(f.connectP2P, "SteamAPI_ISteamNetworkingSockets_ConnectP2P");
                ok &= get(f.accept, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
                ok &= get(f.close, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
                ok &= get(f.closeListen, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
                ok &= get(f.send, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
                ok &= get(f.recvGroup, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");
                ok &= get(f.recvConn, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
                ok &= get(f.createGroup, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup");
                ok &= get(f.destroyGroup, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");
                ok &= get(f.setGroup, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
                ok &= get(f.runCallbacks, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
                ok &= get(f.info, "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo");
                ok &= get(f.realtime, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
                f.complete = ok;
                return f;
            }();
            return flat;
        }

        std::mutex g_liveMutex;
        std::unordered_set<SteamTransport*> g_live;

        void StatusTrampoline(SteamNetConnectionStatusChangedCallback_t* info)
        {
            auto* self = reinterpret_cast<SteamTransport*>(static_cast<std::intptr_t>(info->m_info.m_nUserData));
            std::scoped_lock lock(g_liveMutex);
            if (g_live.contains(self)) {
                self->QueueStatusChange({ info->m_hConn, info->m_info.m_eState, info->m_info.m_hListenSocket,
                                          info->m_info.m_eEndReason, info->m_info.m_szEndDebug });
            }
        }

        std::vector<SteamNetworkingConfigValue_t> Options(const SteamTransport* self, std::size_t maxMessageBytes)
        {
            std::vector<SteamNetworkingConfigValue_t> o(3);
            o[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&StatusTrampoline));
            o[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                          static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(self)));
            o[2].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, static_cast<std::int32_t>(maxMessageBytes));
            return o;
        }
    }

    SteamTransport::SteamTransport(std::size_t maxMessageBytes) : _maxMessageBytes(maxMessageBytes)
    {
        const auto& api = Api();
        if (!api.complete) {
            _initError = "Steam networking unavailable (steam_api64.dll not loaded)";
            return;
        }
        _sockets = api.sockets();
        if (!_sockets) {
            _initError = "Steam networking unavailable (Steam not running?)";
            return;
        }
        if (void* utils = api.utils()) api.initRelay(utils);  // start relay access early (faster first connect)
        _pollGroup = api.createGroup(_sockets);
        {
            std::scoped_lock lock(g_liveMutex);
            g_live.insert(this);
        }
        _ready = true;
    }

    SteamTransport::~SteamTransport()
    {
        if (!_ready) return;
        Shutdown();
        {
            std::scoped_lock lock(g_liveMutex);
            g_live.erase(this);
        }
        Api().destroyGroup(_sockets, _pollGroup);
    }

    bool SteamTransport::Listen(std::uint16_t, std::string& error)
    {
        if (!_ready) {
            error = _initError;
            return false;
        }
        if (_listenSocket != k_HSteamListenSocket_Invalid) return true;
        const auto opts = Options(this, _maxMessageBytes);
        _listenSocket = Api().listenP2P(_sockets, 0, static_cast<int>(opts.size()), opts.data());
        if (_listenSocket == k_HSteamListenSocket_Invalid) {
            error = "could not open a Steam P2P listen socket";
            return false;
        }
        return true;
    }

    ConnectionId SteamTransport::Connect(const Endpoint& endpoint, std::string& error)
    {
        if (!_ready) {
            error = _initError;
            return kInvalidConnection;
        }
        constexpr std::string_view kPrefix = "steam:";
        std::uint64_t id = 0;
        const std::string_view digits = std::string_view(endpoint.host).substr(kPrefix.size());
        if (!endpoint.host.starts_with(kPrefix) ||
            std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc{} || id == 0) {
            error = std::format("'{}' is not a Steam ID", endpoint.host);
            return kInvalidConnection;
        }
        SteamNetworkingIdentity identity;
        identity.Clear();
        identity.SetSteamID64(id);
        const auto opts = Options(this, _maxMessageBytes);
        const auto conn = Api().connectP2P(_sockets, &identity, endpoint.port, static_cast<int>(opts.size()), opts.data());
        if (conn == k_HSteamNetConnection_Invalid) {
            error = std::format("could not start a Steam connection to {}", endpoint.host);
            return kInvalidConnection;
        }
        Api().setGroup(_sockets, conn, _pollGroup);
        _connections[conn] = ConnState{ true, false };
        return conn;
    }

    bool SteamTransport::Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery)
    {
        const auto conn = static_cast<HSteamNetConnection>(connection);
        if (!_connections.contains(conn)) return false;
        const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
        return Api().send(_sockets, conn, data.data(), static_cast<std::uint32_t>(data.size()), flags, nullptr) == k_EResultOK;
    }

    void SteamTransport::Close(ConnectionId connection, CloseCode code, std::string_view reason, bool linger)
    {
        const auto conn = static_cast<HSteamNetConnection>(connection);
        if (_connections.erase(conn) == 0) return;
        const std::string text(reason);
        Api().close(_sockets, conn, static_cast<int>(code), text.c_str(), linger);
        _pending.push_back({ TransportEventType::Disconnected, connection, {}, static_cast<int>(code), text, true });
    }

    void SteamTransport::QueueStatusChange(StatusChange change)
    {
        std::scoped_lock lock(_statusMutex);
        _statusQueue.push_back(std::move(change));
    }

    void SteamTransport::ApplyStatusChange(const StatusChange& change)
    {
        const auto& api = Api();
        const auto conn = change.connection;
        switch (change.state) {
        case k_ESteamNetworkingConnectionState_Connecting:
            if (change.listenSocket != k_HSteamListenSocket_Invalid) {
                if (change.listenSocket != _listenSocket || api.accept(_sockets, conn) != k_EResultOK) {
                    api.close(_sockets, conn, static_cast<int>(CloseCode::ServerShutdown), "not accepting", false);
                    break;
                }
                api.setGroup(_sockets, conn, _pollGroup);
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
            if (it == _connections.end()) break;
            if (it->second.announced || it->second.outgoing) {
                DrainConnection(conn);
                _pending.push_back({ TransportEventType::Disconnected, conn, {}, change.endReason, change.endDebug, false });
            }
            _connections.erase(it);
            api.close(_sockets, conn, 0, nullptr, false);
            break;
        }
        default:
            break;
        }
    }

    void SteamTransport::DrainConnection(std::uint32_t connection)
    {
        SteamNetworkingMessage_t* msgs[32];
        int n = 0;
        while ((n = Api().recvConn(_sockets, connection, msgs, 32)) > 0) {
            for (int i = 0; i < n; ++i) {
                const auto size = static_cast<std::size_t>(msgs[i]->m_cbSize);
                if (size <= _maxMessageBytes) {
                    const auto* p = static_cast<const std::uint8_t*>(msgs[i]->m_pData);
                    _pending.push_back({ TransportEventType::Message, connection, { p, p + size } });
                }
                msgs[i]->Release();
            }
        }
    }

    void SteamTransport::Poll(std::vector<TransportEvent>& out)
    {
        if (!_ready) return;
        const auto& api = Api();
        api.runCallbacks(_sockets);
        std::vector<StatusChange> changes;
        {
            std::scoped_lock lock(_statusMutex);
            changes.swap(_statusQueue);
        }
        for (const auto& c : changes) ApplyStatusChange(c);

        SteamNetworkingMessage_t* msgs[64];
        int n = 0;
        while ((n = api.recvGroup(_sockets, _pollGroup, msgs, 64)) > 0) {
            for (int i = 0; i < n; ++i) {
                auto* m = msgs[i];
                if (auto it = _connections.find(m->m_conn); it != _connections.end()) {
                    if (!it->second.announced) {
                        it->second.announced = true;
                        _pending.push_back({ TransportEventType::Connected, m->m_conn });
                    }
                    const auto size = static_cast<std::size_t>(m->m_cbSize);
                    if (size > _maxMessageBytes) {
                        Close(m->m_conn, CloseCode::Oversize, "message too large", false);
                    } else {
                        const auto* p = static_cast<const std::uint8_t*>(m->m_pData);
                        _pending.push_back({ TransportEventType::Message, m->m_conn, { p, p + size } });
                    }
                }
                m->Release();
            }
        }
        for (auto& ev : _pending) out.push_back(std::move(ev));
        _pending.clear();
    }

    void SteamTransport::Shutdown()
    {
        if (!_ready) return;
        for (const auto& [conn, state] : _connections) {
            Api().close(_sockets, conn, static_cast<int>(CloseCode::ServerShutdown), "shutdown", true);
        }
        _connections.clear();
        if (_listenSocket != k_HSteamListenSocket_Invalid) {
            Api().closeListen(_sockets, _listenSocket);
            _listenSocket = k_HSteamListenSocket_Invalid;
        }
    }

    std::optional<ConnectionStats> SteamTransport::GetStats(ConnectionId connection)
    {
        const auto conn = static_cast<HSteamNetConnection>(connection);
        if (!_connections.contains(conn)) return std::nullopt;
        SteamNetConnectionRealTimeStatus_t status{};
        if (Api().realtime(_sockets, conn, &status, 0, nullptr) != k_EResultOK) return std::nullopt;
        return ConnectionStats{ status.m_nPing, status.m_flConnectionQualityLocal, status.m_cbPendingReliable };
    }

    std::string SteamTransport::Describe(ConnectionId connection)
    {
        SteamNetConnectionInfo_t info{};
        if (!_ready || !Api().info(_sockets, static_cast<HSteamNetConnection>(connection), &info)) {
            return std::format("steam#{}", connection);
        }
        return std::format("steam {}", info.m_szConnectionDescription);
    }

    // ---- FakeSteamTransport -----------------------------------------------------------------------

    bool FakeSteamTransport::Listen(std::uint16_t, std::string& error)
    {
        if (!IsReady()) {
            error = "fake Steam unavailable";
            return false;
        }
        return _gns.Listen(static_cast<std::uint16_t>(kFakeSteamBasePort + _id), error);
    }

    ConnectionId FakeSteamTransport::Connect(const Endpoint& endpoint, std::string& error)
    {
        std::uint32_t target = 0;
        const std::string_view host(endpoint.host);
        const std::string_view digits = host.size() > 6 ? host.substr(6) : std::string_view();
        if (!host.starts_with("steam:") ||
            std::from_chars(digits.data(), digits.data() + digits.size(), target).ec != std::errc{} || target == 0 ||
            target >= 1000) {
            error = std::format("'{}' is not a fake Steam ID (steam:1..999)", endpoint.host);
            return kInvalidConnection;
        }
        return _gns.Connect(Endpoint{ "127.0.0.1", static_cast<std::uint16_t>(kFakeSteamBasePort + target) }, error);
    }

    // ---- MultiTransport -------------------------------------------------------------------------

    bool MultiTransport::Listen(std::uint16_t port, std::string& error)
    {
        if (!_direct.Listen(port, error)) return false;
        std::string p2pError;
        _steamListening = _steam && _steam->Listen(port, p2pError);
        if (_steam && !_steamListening) log::Warn("P2P (Steam) listening failed: {}", p2pError);
        if (_steamListening) log::Info("listening on P2P (Steam) as well as UDP {}", port);
        return true;
    }

    ConnectionId MultiTransport::Connect(const Endpoint& endpoint, std::string& error)
    {
        if (endpoint.host.starts_with("steam:")) {
            if (!_steam) {
                error = "Steam is not available";
                return kInvalidConnection;
            }
            const auto id = _steam->Connect(endpoint, error);
            return id == kInvalidConnection ? kInvalidConnection : (id | kSteamBit);
        }
        return _direct.Connect(endpoint, error);
    }

    bool MultiTransport::Send(ConnectionId c, std::span<const std::uint8_t> data, Delivery delivery)
    {
        if (c & kSteamBit) return _steam && _steam->Send(c & ~kSteamBit, data, delivery);
        return _direct.Send(c, data, delivery);
    }

    void MultiTransport::Close(ConnectionId c, CloseCode code, std::string_view reason, bool linger)
    {
        if (c & kSteamBit) {
            if (_steam) _steam->Close(c & ~kSteamBit, code, reason, linger);
        } else {
            _direct.Close(c, code, reason, linger);
        }
    }

    void MultiTransport::Poll(std::vector<TransportEvent>& out)
    {
        _direct.Poll(out);
        if (!_steam) return;
        _scratch.clear();
        _steam->Poll(_scratch);
        for (auto& ev : _scratch) {
            ev.connection |= kSteamBit;
            out.push_back(std::move(ev));
        }
    }

    void MultiTransport::Shutdown()
    {
        _direct.Shutdown();
        if (_steam) _steam->Shutdown();
    }

    std::optional<ConnectionStats> MultiTransport::GetStats(ConnectionId c)
    {
        if (c & kSteamBit) return _steam ? _steam->GetStats(c & ~kSteamBit) : std::nullopt;
        return _direct.GetStats(c);
    }

    std::string MultiTransport::Describe(ConnectionId c)
    {
        if (c & kSteamBit) return _steam ? _steam->Describe(c & ~kSteamBit) : std::string("steam?");
        return _direct.Describe(c);
    }
}
