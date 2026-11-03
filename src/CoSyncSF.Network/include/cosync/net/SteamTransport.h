#pragma once

#include "cosync/net/GnsTransport.h"
#include "cosync/net/Transport.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace cosync::net
{
    // ITransport over Steam's own ISteamNetworkingSockets (P2P by SteamID, routed through Valve's
    // relay network: no port forwarding, NAT-safe). Uses the steam_api64.dll already loaded by the
    // game, through its flat API resolved at runtime, so binaries without Steam (dedicated server,
    // tests) still load; there IsReady() is false.
    //
    // The struct layouts come from the GameNetworkingSockets headers: both are interface version
    // SteamNetworkingSockets012 / SteamNetworkingUtils004 (checked against the game's DLL exports).
    //
    // Endpoints: host "steam:<SteamID64>", port = remote virtual port (0). Listen(port) ignores the
    // UDP port and listens on virtual port 0.
    class SteamTransport final : public ITransport
    {
    public:
        explicit SteamTransport(std::size_t maxMessageBytes = 16 * 1024);
        ~SteamTransport() override;

        SteamTransport(const SteamTransport&) = delete;
        SteamTransport& operator=(const SteamTransport&) = delete;

        [[nodiscard]] bool IsReady() const noexcept { return _ready; }
        [[nodiscard]] const std::string& InitError() const noexcept { return _initError; }

        bool Listen(std::uint16_t port, std::string& error) override;
        ConnectionId Connect(const Endpoint& endpoint, std::string& error) override;
        bool Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery) override;
        void Close(ConnectionId connection, CloseCode code, std::string_view reason, bool linger) override;
        void Poll(std::vector<TransportEvent>& out) override;
        void Shutdown() override;
        std::optional<ConnectionStats> GetStats(ConnectionId connection) override;
        [[nodiscard]] std::string Describe(ConnectionId connection) override;

        struct StatusChange
        {
            std::uint32_t connection = 0;
            int state = 0;
            std::uint32_t listenSocket = 0;
            int endReason = 0;
            std::string endDebug;
        };
        void QueueStatusChange(StatusChange change);

    private:
        struct ConnState
        {
            bool outgoing = false;
            bool announced = false;
        };
        void ApplyStatusChange(const StatusChange& change);
        void DrainConnection(std::uint32_t connection);

        std::size_t _maxMessageBytes;
        bool _ready = false;
        std::string _initError;
        void* _sockets = nullptr;  // ISteamNetworkingSockets*
        std::uint32_t _listenSocket = 0;
        std::uint32_t _pollGroup = 0;
        std::unordered_map<std::uint32_t, ConnState> _connections;
        std::vector<TransportEvent> _pending;
        std::mutex _statusMutex;
        std::vector<StatusChange> _statusQueue;
    };

    // Development stand-in for Steam P2P, for testing several game copies on one PC with one Steam
    // account: identity "steam:<N>" (small N from [Dev] FakeSteamId), carried over a local
    // GameNetworkingSockets socket on UDP kFakeSteamBasePort + N. Steam itself is not involved.
    inline constexpr std::uint16_t kFakeSteamBasePort = 47000;
    class FakeSteamTransport final : public ITransport
    {
    public:
        explicit FakeSteamTransport(std::uint32_t fakeId) : _id(fakeId) {}
        [[nodiscard]] bool IsReady() const noexcept { return _gns.IsReady() && _id != 0 && _id < 1000; }
        bool Listen(std::uint16_t port, std::string& error) override;
        ConnectionId Connect(const Endpoint& endpoint, std::string& error) override;
        bool Send(ConnectionId c, std::span<const std::uint8_t> data, Delivery d) override { return _gns.Send(c, data, d); }
        void Close(ConnectionId c, CloseCode code, std::string_view reason, bool linger) override
        {
            _gns.Close(c, code, reason, linger);
        }
        void Poll(std::vector<TransportEvent>& out) override { _gns.Poll(out); }
        void Shutdown() override { _gns.Shutdown(); }
        std::optional<ConnectionStats> GetStats(ConnectionId c) override { return _gns.GetStats(c); }
        [[nodiscard]] std::string Describe(ConnectionId c) override { return "fake-steam " + _gns.Describe(c); }

    private:
        std::uint32_t _id;
        GnsTransport _gns;
    };

    // Routes between a direct-IP transport and a P2P transport (Steam, or FakeSteamTransport):
    // endpoints with host "steam:..." go to P2P, everything else to `direct`. Listen() starts both
    // (P2P failing is not fatal). P2P connection ids carry bit 63. `p2p` may be null.
    class MultiTransport final : public ITransport
    {
    public:
        MultiTransport(ITransport& direct, ITransport* p2p) : _direct(direct), _steam(p2p) {}

        bool Listen(std::uint16_t port, std::string& error) override;
        ConnectionId Connect(const Endpoint& endpoint, std::string& error) override;
        bool Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery) override;
        void Close(ConnectionId connection, CloseCode code, std::string_view reason, bool linger) override;
        void Poll(std::vector<TransportEvent>& out) override;
        void Shutdown() override;
        std::optional<ConnectionStats> GetStats(ConnectionId connection) override;
        [[nodiscard]] std::string Describe(ConnectionId connection) override;
        [[nodiscard]] bool SteamListening() const noexcept { return _steamListening; }

    private:
        static constexpr ConnectionId kSteamBit = 1ull << 63;
        ITransport& _direct;
        ITransport* _steam;
        bool _steamListening = false;
        std::vector<TransportEvent> _scratch;
    };
}
