#include "cosync/net/NetworkWorker.h"

#include "cosync/net/GnsTransport.h"
#include "cosync/net/LanDiscovery.h"
#include "cosync/shared/Log.h"

#include <chrono>
#include <algorithm>

namespace cosync::net
{
    NetworkWorker::NetworkWorker(WorkerConfig config) :
        _config(std::move(config)), _events(_config.eventQueueCapacity)
    {}

    NetworkWorker::~NetworkWorker() { Stop(); }

    bool NetworkWorker::Start(std::string& error)
    {
        if (_thread.joinable()) {
            error = "already started";
            return false;
        }
        _stopRequested = false;
        // The promise is owned by the thread so it outlives set_value() there.
        std::promise<std::string> ready;
        auto result = ready.get_future();
        _thread = std::thread([this, p = std::move(ready)]() mutable { Run(p); });
        error = result.get();
        if (!error.empty()) {
            _thread.join();
            return false;
        }
        return true;
    }

    void NetworkWorker::Stop()
    {
        _stopRequested = true;
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    std::size_t NetworkWorker::DrainEvents(std::vector<ClientEvent>& out, std::size_t max)
    {
        return _events.Drain(out, max);
    }

    void NetworkWorker::TakeRemoteTransforms(std::vector<proto::PlayerTransform>& out) { _remoteTransforms.TakeAll(out); }

    void NetworkWorker::SubmitLocalTransform(std::uint32_t locationId, const Vec3& position, const Vec3& rotation)
    {
        std::scoped_lock lock(_localMutex);
        _localTransform = LocalTransform{ locationId, position, rotation };
    }

    std::map<PlayerId, std::string> NetworkWorker::Roster() const
    {
        std::scoped_lock lock(_statusMutex);
        return _roster;
    }

    WorkerStatus NetworkWorker::Status() const
    {
        std::scoped_lock lock(_statusMutex);
        return _status;
    }

    void NetworkWorker::Run(std::promise<std::string>& ready)
    {
        // Everything network-related is created, used and destroyed on this thread.
        GnsTransport serverTransport;
        GnsTransport clientTransport;
        std::optional<SessionServer> server;
        discovery::Responder discovery;

        if (!clientTransport.IsReady()) {
            ready.set_value(clientTransport.InitError());
            return;
        }

        Endpoint target = _config.connectTo;
        std::string error;
        if (_config.mode == WorkerMode::Host) {
            server.emplace(serverTransport, _config.server);
            if (!server->Start(_config.listenPort, error)) {
                ready.set_value(error);
                return;
            }
            if (_config.discoveryPort != 0 && !discovery.Start(_config.discoveryPort, error)) {
                log::Warn("LAN discovery disabled: {}", error);
            }
            target = Endpoint{ "127.0.0.1", _config.listenPort };
        }

        SessionClient client(clientTransport, _config.client);
        if (!client.Connect(target, error)) {
            ready.set_value(error);
            return;
        }

        _running = true;
        ready.set_value({});  // `ready` must not be touched after this point

        const auto tick = std::chrono::microseconds(1'000'000 / std::max<std::uint32_t>(_config.tickHz, 1));
        auto nextTick = std::chrono::steady_clock::now();
        std::vector<ClientEvent> events;
        std::uint64_t dropped = 0;

        while (!_stopRequested) {
            if (server) {
                server->Update();
                discovery::ServerInfo info;
                info.protocolMajor = proto::kProtocolMajor;
                info.protocolMinor = proto::kProtocolMinor;
                info.gamePort = _config.listenPort;
                info.players = static_cast<std::uint16_t>(server->ConnectedPlayerCount());
                info.maxPlayers = server->Config().maxPlayers;
                info.requiredGameRuntime = server->Config().requiredGameRuntime;
                info.passwordRequired = !server->Config().password.empty();
                info.serverName = server->Config().serverName;
                discovery.Poll(info);
            }

            {
                std::optional<LocalTransform> local;
                {
                    std::scoped_lock lock(_localMutex);
                    local.swap(_localTransform);
                }
                if (local) {
                    client.SendTransform(local->locationId, local->position, local->rotation);
                }
            }

            events.clear();
            client.Update(events);
            for (auto& ev : events) {
                if (auto* t = std::get_if<client_event::RemoteTransform>(&ev)) {
                    _remoteTransforms.Put(t->transform.playerId, t->transform);
                    continue;
                }
                if (auto* left = std::get_if<client_event::PeerLeft>(&ev)) {
                    _remoteTransforms.Erase(left->playerId);
                }
                if (!_events.TryPush(std::move(ev))) {
                    ++dropped;
                    _overflow = true;
                }
            }

            {
                std::scoped_lock lock(_statusMutex);
                _status.state = client.State();
                _status.localPlayer = client.LocalPlayerId();
                _status.rttMs = client.LastRttMs();
                _status.peers = client.Roster().size();
                _status.droppedEvents = dropped;
                _roster = client.Roster();
            }

            nextTick += tick;
            std::this_thread::sleep_until(nextTick);
        }

        // Graceful teardown on this thread, then a short flush so the server and
        // peers see a clean leave instead of a timeout.
        client.Disconnect();
        if (server) {
            server->Stop();
        }
        for (int i = 0; i < 10; ++i) {
            events.clear();
            client.Update(events);
            if (server) server->Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        discovery.Stop();
        _running = false;
    }
}
