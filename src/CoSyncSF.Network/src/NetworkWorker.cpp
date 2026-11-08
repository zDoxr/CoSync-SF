#include "cosync/net/NetworkWorker.h"

#include "cosync/net/GnsTransport.h"
#include "cosync/net/SteamTransport.h"

#include <memory>
#include "cosync/net/LanDiscovery.h"
#include "cosync/shared/Log.h"
#include "cosync/shared/NpcReplication.h"

#include <chrono>
#include <algorithm>
#include <iterator>

namespace cosync::net
{
    NetworkWorker::NetworkWorker(WorkerConfig config) :
        _config(std::move(config)), _events(_config.eventQueueCapacity), _remoteTransforms(_config.transformQueueCapacity)
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

    void NetworkWorker::TakeRemoteTransforms(std::vector<RemoteSample>& out)
    {
        _remoteTransforms.Drain(out, _remoteTransforms.Capacity());
    }

    void NetworkWorker::SubmitLocalTransform(std::uint32_t locationId, const Vec3& position, const Vec3& rotation,
                                             const std::optional<proto::PlayerControls>& controls,
                                             const std::vector<proto::AnimEvent>& events,
                                             const std::optional<proto::WeaponState>& weapon,
                                             const std::optional<proto::PlayerStatus>& status,
                                             const std::optional<proto::ShipState>& ship)
    {
        std::scoped_lock lock(_localMutex);
        _localTransform = LocalTransform{ locationId, position, rotation, controls, events, weapon, status, ship };
    }

    void NetworkWorker::SubmitNpcCombat(std::uint32_t npcFormId, PlayerId targetPlayer)
    {
        std::scoped_lock lock(_localMutex);
        if (_npcCombatOut.size() < 256) _npcCombatOut.emplace_back(npcFormId, targetPlayer);
    }

    void NetworkWorker::SubmitNpcStates(std::uint32_t locationId, std::vector<proto::NpcState> npcs)
    {
        std::scoped_lock lock(_localMutex);
        _npcStatesOut = proto::NpcStates{ kInvalidPlayerId, 0, locationId, std::move(npcs) };
    }

    void NetworkWorker::SubmitNpcDeath(std::uint32_t npcFormId)
    {
        std::scoped_lock lock(_localMutex);
        if (_npcDeathOut.size() < 256) _npcDeathOut.push_back(npcFormId);
    }

    void NetworkWorker::SubmitNpcDamage(std::uint32_t npcFormId, float amount)
    {
        std::scoped_lock lock(_localMutex);
        if (_npcDamageOut.size() < 256) _npcDamageOut.emplace_back(npcFormId, amount);
    }

    void NetworkWorker::SubmitNpcEvents(std::vector<proto::NpcEvent> events)
    {
        constexpr std::size_t kMaxQueued = 512;
        std::scoped_lock lock(_localMutex);
        for (auto& e : events) {
            if (_npcEventsOut.size() >= kMaxQueued) _npcEventsOut.pop_front();
            _npcEventsOut.push_back(std::move(e));
        }
    }

    void NetworkWorker::SubmitNpcOutfits(std::vector<proto::NpcOutfit> outfits)
    {
        std::scoped_lock lock(_localMutex);
        _npcOutfitsOut = std::move(outfits);  // full state: a newer one replaces an unsent one
    }

    void NetworkWorker::SubmitWorldEvents(const std::vector<proto::WorldEvent>& events)
    {
        constexpr std::size_t kMaxQueued = 4096;
        std::scoped_lock lock(_localMutex);
        for (const auto& e : events) {
            if (_worldEventsOut.size() < kMaxQueued) _worldEventsOut.push_back(e);
        }
    }

    void NetworkWorker::SubmitDroppedItem(const proto::DroppedItem& drop)
    {
        std::scoped_lock lock(_localMutex);
        if (_dropsOut.size() >= 64) _dropsOut.pop_front();
        _dropsOut.push_back(drop);
    }

    void NetworkWorker::SubmitDialogueLine(const proto::DialogueLine& line)
    {
        std::scoped_lock lock(_localMutex);
        if (_dialogueOut.size() >= 32) _dialogueOut.pop_front();
        _dialogueOut.push_back(line);
    }

    void NetworkWorker::SubmitWorldClock(const proto::WorldClock& clock)
    {
        std::scoped_lock lock(_localMutex);
        _worldClockOut = clock;
    }

    std::vector<proto::NpcStates> NetworkWorker::TakeNpcStates()
    {
        std::scoped_lock lock(_localMutex);
        std::vector<proto::NpcStates> out(std::make_move_iterator(_npcStatesIn.begin()),
                                          std::make_move_iterator(_npcStatesIn.end()));
        _npcStatesIn.clear();
        return out;
    }

    void NetworkWorker::SubmitGroupRequest(const proto::GroupRequest& request)
    {
        std::scoped_lock lock(_localMutex);
        if (_groupOut.size() < 64) _groupOut.push_back(request);
    }

    void NetworkWorker::SubmitKick(PlayerId player)
    {
        std::scoped_lock lock(_localMutex);
        if (_kickOut.size() < 64) _kickOut.push_back(player);
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
        GnsTransport serverUdp;
        GnsTransport clientUdp;
        // Steam P2P/relay alongside direct UDP (inactive outside the game: no steam_api64.dll).
        std::unique_ptr<ITransport> serverP2P;
        std::unique_ptr<ITransport> clientP2P;
        if (_config.fakeSteamId != 0) {
            auto s = std::make_unique<FakeSteamTransport>(_config.fakeSteamId);
            auto c = std::make_unique<FakeSteamTransport>(_config.fakeSteamId);
            if (s->IsReady() && c->IsReady()) {
                serverP2P = std::move(s);
                clientP2P = std::move(c);
            }
        } else {
            auto s = std::make_unique<SteamTransport>();
            auto c = std::make_unique<SteamTransport>();
            if (s->IsReady() && c->IsReady()) {
                serverP2P = std::move(s);
                clientP2P = std::move(c);
            }
        }
        MultiTransport serverTransport(serverUdp, serverP2P.get());
        MultiTransport clientTransport(clientUdp, clientP2P.get());
        std::optional<SessionServer> server;
        discovery::Responder discovery;

        if (!clientUdp.IsReady()) {
            ready.set_value(clientUdp.InitError());
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
        auto nextStats = std::chrono::steady_clock::now();

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
                if (const auto now = std::chrono::steady_clock::now(); now >= nextStats) {
                    nextStats = now + std::chrono::seconds(30);
                    const auto& st = server->Stats();
                    log::Info("[server] {} players, {} transforms relayed, {} NPC messages refused, {} NPC messages not sent to players elsewhere, {} dialogue lines, {} pickups given back, {} drops",
                              server->ConnectedPlayerCount(), st.transformsRelayed, st.npcMessagesRefused, st.npcMessagesNotRelevant, st.dialogueLinesRelayed,
                              st.itemsReturned, st.dropsRelayed);
                }
            }

            {
                static thread_local std::uint32_t npcEventTick = 0;
                std::optional<LocalTransform> local;
                {
                    std::scoped_lock lock(_localMutex);
                    local.swap(_localTransform);
                }
                std::vector<std::pair<std::uint32_t, PlayerId>> combat;
                {
                    std::scoped_lock lock(_localMutex);
                    combat.swap(_npcCombatOut);
                }
                for (const auto& [npc, targetPlayer] : combat) client.SendNpcCombat(npc, targetPlayer);
                std::vector<proto::GroupRequest> groupRequests;
                {
                    std::scoped_lock lock(_localMutex);
                    groupRequests.swap(_groupOut);
                }
                for (const auto& g : groupRequests) client.SendGroupRequest(g);
                std::optional<proto::NpcStates> npcStates;
                std::vector<std::uint32_t> npcDeaths;
                std::vector<std::pair<std::uint32_t, float>> npcDamage;
                {
                    std::scoped_lock lock(_localMutex);
                    npcStates.swap(_npcStatesOut);
                    npcDeaths.swap(_npcDeathOut);
                    npcDamage.swap(_npcDamageOut);
                }
                std::vector<proto::NpcOutfit> npcOutfits;
                std::vector<proto::NpcEvent> npcEvents;
                {
                    std::scoped_lock lock(_localMutex);
                    npcOutfits.swap(_npcOutfitsOut);
                    // One message every sixth tick (10 Hz at the default rate). With position at 60 Hz,
                    // snapshots at 30 and outfits at 5 this stays well under what the server allows.
                    const auto n = ++npcEventTick % 6 == 0 ? std::min(_npcEventsOut.size(), proto::kMaxNpcEvents)
                                                           : std::size_t{ 0 };
                    npcEvents.assign(std::make_move_iterator(_npcEventsOut.begin()),
                                     std::make_move_iterator(_npcEventsOut.begin() + static_cast<std::ptrdiff_t>(n)));
                    _npcEventsOut.erase(_npcEventsOut.begin(), _npcEventsOut.begin() + static_cast<std::ptrdiff_t>(n));
                }
                std::vector<proto::WorldEvent> worldEvents;
                std::optional<proto::WorldClock> worldClock;
                std::deque<proto::DialogueLine> dialogue;
                std::deque<proto::DroppedItem> drops;
                {
                    std::scoped_lock lock(_localMutex);
                    // World changes and drops wait while the connection is being resumed (issue #14):
                    // sent then, they would be lost, and the others would never hear of them.
                    if (client.State() == ClientState::InSession) {
                        auto n = static_cast<std::ptrdiff_t>(std::min(_worldEventsOut.size(), proto::kMaxWorldEvents));
                        // A container change and the count after it go in the same message: the server reads them as a pair.
                        if (n > 1 && static_cast<std::size_t>(n) < _worldEventsOut.size() &&
                            _worldEventsOut[static_cast<std::size_t>(n)].kind == proto::WorldEventKind::ContainerRemaining) {
                            --n;
                        }
                        worldEvents.assign(_worldEventsOut.begin(), _worldEventsOut.begin() + n);
                        _worldEventsOut.erase(_worldEventsOut.begin(), _worldEventsOut.begin() + n);
                        drops.swap(_dropsOut);
                    }
                    worldClock.swap(_worldClockOut);
                    dialogue.swap(_dialogueOut);
                }
                for (const auto& drop : drops) client.SendDroppedItem(drop);
                for (const auto& line : dialogue) client.SendDialogueLine(line);
                client.SendWorldEvents(std::move(worldEvents));
                if (worldClock) client.SendWorldClock(*worldClock);
                for (auto& part : npc::SplitOutfits(std::move(npcOutfits))) client.SendNpcOutfits(std::move(part));
                client.SendNpcEvents(std::move(npcEvents));
                // Deaths first: a snapshot sent after them already shows the NPC dead.
                for (const auto npc : npcDeaths) client.SendNpcDeath(npc);
                for (const auto& [npc, amount] : npcDamage) client.SendNpcDamage(npc, amount);
                if (npcStates) {
                    for (auto& part : npc::SplitForWire(npcStates->npcs)) {
                        client.SendNpcStates(npcStates->locationId, std::move(part));
                    }
                }
                std::vector<PlayerId> kicks;
                {
                    std::scoped_lock lock(_localMutex);
                    kicks.swap(_kickOut);
                }
                if (server) {
                    for (const auto id : kicks) server->Kick(id, "kicked by the host");
                }
                if (local) {
                    client.SendTransform(local->locationId, local->position, local->rotation, local->controls, local->events, local->weapon, local->status, local->ship);
                }
            }

            if (const auto loss = _simulatedLossMs.exchange(0); loss != 0) client.SimulateConnectionLoss(loss);
            events.clear();
            client.Update(events);
            for (auto& ev : events) {
                if (auto* t = std::get_if<client_event::RemoteTransform>(&ev)) {
                    const auto& m = t->transform;
                    _remoteTransforms.PushDropOldest(
                        { m.playerId, { SteadyNowMs(), m.sequence, m.locationId, m.position, m.rotation, m.controls, m.events, m.weapon, m.status, m.ship } });
                    continue;
                }
                if (auto* n = std::get_if<client_event::NpcStates>(&ev)) {
                    constexpr std::size_t kMaxQueuedParts = 256;
                    std::scoped_lock lock(_localMutex);
                    if (_npcStatesIn.size() >= kMaxQueuedParts) _npcStatesIn.pop_front();
                    _npcStatesIn.push_back(std::move(n->states));
                    continue;
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
