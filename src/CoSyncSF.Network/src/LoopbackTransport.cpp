#include "cosync/net/LoopbackTransport.h"

#include <format>

namespace cosync::net
{
    LoopbackTransport::LoopbackTransport(std::shared_ptr<LoopbackHub> hub, std::size_t maxMessageBytes) :
        _hub(std::move(hub)), _maxMessageBytes(maxMessageBytes)
    {}

    LoopbackTransport::~LoopbackTransport() { Shutdown(); }

    bool LoopbackTransport::Listen(std::uint16_t port, std::string& error)
    {
        if (_listenPort != 0) {
            error = "already listening";
            return false;
        }
        if (port == 0 || _hub->_listeners.contains(port)) {
            error = std::format("loopback port {} unavailable", port);
            return false;
        }
        _hub->_listeners[port] = this;
        _listenPort = port;
        return true;
    }

    ConnectionId LoopbackTransport::Connect(const Endpoint& endpoint, std::string&)
    {
        const ConnectionId local = _hub->_nextId++;
        const auto it = _hub->_listeners.find(endpoint.port);
        if (it == _hub->_listeners.end()) {
            // Report the failure asynchronously, like a real transport would.
            Push({ TransportEventType::Disconnected, local, {}, 0, "connection refused", false });
            return local;
        }

        LoopbackTransport* server = it->second;
        const ConnectionId remote = _hub->_nextId++;
        _hub->_links[local] = { this, remote };
        _hub->_links[remote] = { server, local };
        Push({ TransportEventType::Connected, local });
        server->Push({ TransportEventType::Connected, remote });
        return local;
    }

    bool LoopbackTransport::Send(ConnectionId connection, std::span<const std::uint8_t> data, Delivery delivery)
    {
        const auto it = _hub->_links.find(connection);
        if (it == _hub->_links.end() || it->second.owner != this) {
            return false;
        }
        if (delivery == Delivery::Unreliable && _dropUnreliable) {
            return true;
        }
        const auto peer = it->second.peer;
        auto& peerLink = _hub->_links.at(peer);
        peerLink.owner->Push({ TransportEventType::Message, peer, { data.begin(), data.end() } });
        return true;
    }

    void LoopbackTransport::Close(ConnectionId connection, CloseCode code, std::string_view reason, bool)
    {
        const auto it = _hub->_links.find(connection);
        if (it == _hub->_links.end() || it->second.owner != this) {
            return;
        }
        const auto peer = it->second.peer;
        _hub->_links.erase(it);

        // Messages sent before Close are already queued at the peer ahead of this
        // event, which models GNS linger semantics.
        if (const auto pit = _hub->_links.find(peer); pit != _hub->_links.end()) {
            pit->second.owner->Push(
                { TransportEventType::Disconnected, peer, {}, static_cast<int>(code), std::string(reason), false });
            _hub->_links.erase(pit);
        }
        Push({ TransportEventType::Disconnected, connection, {}, static_cast<int>(code), std::string(reason), true });
    }

    void LoopbackTransport::Poll(std::vector<TransportEvent>& out)
    {
        while (!_pending.empty()) {
            auto ev = std::move(_pending.front());
            _pending.pop_front();
            if (ev.type == TransportEventType::Message && ev.data.size() > _maxMessageBytes) {
                Close(ev.connection, CloseCode::Oversize, "message too large", false);
                continue;
            }
            out.push_back(std::move(ev));
        }
    }

    void LoopbackTransport::Shutdown()
    {
        std::vector<ConnectionId> mine;
        for (const auto& [id, link] : _hub->_links) {
            if (link.owner == this) {
                mine.push_back(id);
            }
        }
        for (auto id : mine) {
            Close(id, CloseCode::ServerShutdown, "transport shutdown", true);
        }
        if (_listenPort != 0) {
            _hub->_listeners.erase(_listenPort);
            _listenPort = 0;
        }
    }

    void LoopbackTransport::SimulateNetworkFailure()
    {
        std::vector<std::pair<ConnectionId, ConnectionId>> mine;
        for (const auto& [id, link] : _hub->_links) {
            if (link.owner == this) {
                mine.emplace_back(id, link.peer);
            }
        }
        for (const auto& [id, peer] : mine) {
            _hub->_links.erase(id);
            Push({ TransportEventType::Disconnected, id, {}, 0, "network failure", false });
            if (const auto pit = _hub->_links.find(peer); pit != _hub->_links.end()) {
                pit->second.owner->Push({ TransportEventType::Disconnected, peer, {}, 0, "network failure", false });
                _hub->_links.erase(pit);
            }
        }
    }

    std::optional<ConnectionStats> LoopbackTransport::GetStats(ConnectionId connection)
    {
        if (!_hub->_links.contains(connection)) {
            return std::nullopt;
        }
        return ConnectionStats{ 0, 1.0f, 0 };
    }

    std::string LoopbackTransport::Describe(ConnectionId connection) { return std::format("loopback#{}", connection); }
}
