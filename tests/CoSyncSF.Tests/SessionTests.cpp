#include "cosync/net/LoopbackTransport.h"
#include "cosync/net/SessionClient.h"
#include "cosync/net/SessionServer.h"
#include "cosync/shared/ByteStream.h"
#include "cosync/shared/GameRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

using namespace cosync;
using namespace cosync::net;

namespace
{
    constexpr std::uint16_t kPort = 7777;

    template <class T>
    std::vector<T> Collect(const std::vector<ClientEvent>& events)
    {
        std::vector<T> out;
        for (const auto& e : events) {
            if (auto* p = std::get_if<T>(&e)) out.push_back(*p);
        }
        return out;
    }

    struct Fixture : ::testing::Test
    {
        std::shared_ptr<LoopbackHub> hub = std::make_shared<LoopbackHub>();
        std::uint64_t now = 10'000;
        Clock clock = [this] { return now; };
        std::unique_ptr<LoopbackTransport> serverTransport;
        std::unique_ptr<SessionServer> server;

        struct Client
        {
            std::unique_ptr<LoopbackTransport> transport;
            std::unique_ptr<SessionClient> session;
            std::vector<ClientEvent> events;  // accumulated since last Clear()

            void Clear() { events.clear(); }
        };
        std::vector<std::unique_ptr<Client>> clients;

        // Raw peer for sending arbitrary bytes.
        struct Raw
        {
            std::unique_ptr<LoopbackTransport> transport;
            ConnectionId connection = kInvalidConnection;
            std::vector<TransportEvent> received;

            bool Closed() const
            {
                return std::ranges::any_of(received, [](const TransportEvent& e) {
                    return e.type == TransportEventType::Disconnected;
                });
            }
            std::optional<proto::Reject> LastReject() const
            {
                std::optional<proto::Reject> out;
                for (const auto& e : received) {
                    if (e.type != TransportEventType::Message) continue;
                    auto d = proto::Decode(e.data);
                    if (d.Ok()) {
                        if (auto* r = std::get_if<proto::Reject>(&*d.message)) out = *r;
                    }
                }
                return out;
            }
        };
        std::vector<std::unique_ptr<Raw>> raws;

        void StartServer(ServerConfig config = {})
        {
            serverTransport = std::make_unique<LoopbackTransport>(hub, proto::kMaxMessageBytes);
            server = std::make_unique<SessionServer>(*serverTransport, std::move(config), clock);
            std::string error;
            ASSERT_TRUE(server->Start(kPort, error)) << error;
        }

        Client& AddClient(std::string name, ClientConfig config = {})
        {
            auto c = std::make_unique<Client>();
            c->transport = std::make_unique<LoopbackTransport>(hub, proto::kMaxMessageBytes);
            config.playerName = std::move(name);
            c->session = std::make_unique<SessionClient>(*c->transport, config, clock);
            std::string error;
            EXPECT_TRUE(c->session->Connect({ "127.0.0.1", kPort }, error)) << error;
            clients.push_back(std::move(c));
            return *clients.back();
        }

        Raw& AddRaw()
        {
            auto r = std::make_unique<Raw>();
            r->transport = std::make_unique<LoopbackTransport>(hub, proto::kMaxMessageBytes);
            std::string error;
            r->connection = r->transport->Connect({ "127.0.0.1", kPort }, error);
            raws.push_back(std::move(r));
            return *raws.back();
        }

        void SendRaw(Raw& r, const proto::Message& msg) { r.transport->Send(r.connection, proto::Encode(msg), Delivery::Reliable); }

        void Pump(int rounds = 4)
        {
            for (int i = 0; i < rounds; ++i) {
                if (server) server->Update();
                for (auto& c : clients) c->session->Update(c->events);
                for (auto& r : raws) r->transport->Poll(r->received);
            }
        }

        void Advance(std::uint64_t ms, std::uint64_t step = 250)
        {
            for (std::uint64_t t = 0; t < ms; t += step) {
                now += step;
                Pump(1);
            }
            Pump();
        }

        static proto::Hello ValidHello(std::string name)
        {
            proto::Hello h;
            h.playerName = std::move(name);
            return h;
        }
    };
}

TEST_F(Fixture, TwoPlayersJoinAndSeeEachOther)
{
    StartServer();
    auto& a = AddClient("Alice");
    Pump();
    ASSERT_EQ(a.session->State(), ClientState::InSession);
    EXPECT_EQ(a.session->LocalPlayerId(), 1u);
    EXPECT_TRUE(a.session->Roster().empty());

    auto& b = AddClient("Bob");
    Pump();
    ASSERT_EQ(b.session->State(), ClientState::InSession);
    EXPECT_EQ(b.session->LocalPlayerId(), 2u);

    // Bob learned about Alice from the snapshot; Alice from PlayerJoined.
    ASSERT_EQ(b.session->Roster().size(), 1u);
    EXPECT_EQ(b.session->Roster().at(1), "Alice");
    ASSERT_EQ(a.session->Roster().size(), 1u);
    EXPECT_EQ(a.session->Roster().at(2), "Bob");
    const auto joined = Collect<client_event::PeerJoined>(a.events);
    ASSERT_EQ(joined.size(), 1u);
    EXPECT_EQ(joined[0].player.name, "Bob");
    EXPECT_EQ(server->ConnectedPlayerCount(), 2u);
}

TEST_F(Fixture, HeartbeatsProduceRttAndKeepSessionAlive)
{
    StartServer();
    auto& a = AddClient("Alice");
    Pump();
    Advance(30'000);  // far beyond idle timeout, but heartbeats flow
    EXPECT_EQ(a.session->State(), ClientState::InSession);
    EXPECT_FALSE(Collect<client_event::RttSample>(a.events).empty());
    EXPECT_EQ(server->ConnectedPlayerCount(), 1u);
}

TEST_F(Fixture, TransformRelayStampsIdentityAndDropsStale)
{
    StartServer();
    auto& a = AddClient("Alice");
    auto& b = AddClient("Bob");
    Pump();
    b.Clear();

    ASSERT_TRUE(a.session->SendTransform(7, { 1, 2, 3 }, { 0, 0, 1 }));
    Pump();
    auto got = Collect<client_event::RemoteTransform>(b.events);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].transform.playerId, a.session->LocalPlayerId());
    EXPECT_EQ(got[0].transform.locationId, 7u);
    EXPECT_FLOAT_EQ(got[0].transform.position.z, 3.0f);
    EXPECT_TRUE(Collect<client_event::RemoteTransform>(a.events).empty());  // not echoed to sender

    // A raw replay of an old sequence number is dropped by the server.
    const auto before = server->Stats().staleTransformsDropped;
    auto& raw = AddRaw();
    Pump();
    SendRaw(raw, ValidHello("Mallory"));
    Pump();
    proto::PlayerTransform t;
    t.sequence = 10;
    SendRaw(raw, t);
    t.sequence = 9;
    SendRaw(raw, t);
    Pump();
    EXPECT_EQ(server->Stats().staleTransformsDropped, before + 1);
}

TEST_F(Fixture, RejectsProtocolMajorMismatch)
{
    StartServer();
    auto& raw = AddRaw();
    Pump();
    auto hello = ValidHello("Future");
    hello.protocolMajor = proto::kProtocolMajor + 1;
    SendRaw(raw, hello);
    Pump();
    const auto reject = raw.LastReject();
    ASSERT_TRUE(reject);
    EXPECT_EQ(reject->reason, proto::RejectReason::ProtocolVersionMismatch);
    EXPECT_EQ(reject->serverProtocolMajor, proto::kProtocolMajor);
    EXPECT_TRUE(raw.Closed());
    EXPECT_EQ(server->ConnectedPlayerCount(), 0u);
}

TEST_F(Fixture, RejectsGameVersionMismatch)
{
    ServerConfig cfg;
    cfg.requiredGameRuntime = game::kTargetRuntime;
    StartServer(cfg);

    ClientConfig old;
    old.gameRuntime = game::MakeRuntimeVersion(1, 15, 222);
    auto& a = AddClient("Alice", old);
    Pump();
    EXPECT_EQ(a.session->State(), ClientState::Disconnected);
    const auto rejected = Collect<client_event::Rejected>(a.events);
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0].reject.reason, proto::RejectReason::GameVersionMismatch);

    ClientConfig current;
    current.gameRuntime = game::kTargetRuntime;
    auto& b = AddClient("Bob", current);
    Pump();
    EXPECT_EQ(b.session->State(), ClientState::InSession);
}

TEST_F(Fixture, RejectsBadPassword)
{
    ServerConfig cfg;
    cfg.password = "hunter2";
    StartServer(cfg);
    ClientConfig wrong;
    wrong.password = "hunter3";
    auto& a = AddClient("Alice", wrong);
    ClientConfig right;
    right.password = "hunter2";
    auto& b = AddClient("Bob", right);
    Pump();
    EXPECT_EQ(a.session->State(), ClientState::Disconnected);
    ASSERT_EQ(Collect<client_event::Rejected>(a.events).size(), 1u);
    EXPECT_EQ(Collect<client_event::Rejected>(a.events)[0].reject.reason, proto::RejectReason::BadPassword);
    EXPECT_EQ(b.session->State(), ClientState::InSession);
}

TEST_F(Fixture, RejectsDuplicateNameCaseInsensitive)
{
    StartServer();
    AddClient("Alice");
    Pump();
    auto& dup = AddClient("ALICE");
    Pump();
    ASSERT_EQ(Collect<client_event::Rejected>(dup.events).size(), 1u);
    EXPECT_EQ(Collect<client_event::Rejected>(dup.events)[0].reject.reason, proto::RejectReason::NameInUse);
}

TEST_F(Fixture, RejectsWhenFull)
{
    ServerConfig cfg;
    cfg.maxPlayers = 2;
    StartServer(cfg);
    AddClient("A");
    AddClient("B");
    Pump();
    auto& c = AddClient("C");
    Pump();
    ASSERT_EQ(Collect<client_event::Rejected>(c.events).size(), 1u);
    EXPECT_EQ(Collect<client_event::Rejected>(c.events)[0].reject.reason, proto::RejectReason::ServerFull);
}

TEST_F(Fixture, RejectsInvalidName)
{
    StartServer();
    auto& raw = AddRaw();
    Pump();
    SendRaw(raw, ValidHello(" padded"));
    Pump();
    ASSERT_TRUE(raw.LastReject());
    EXPECT_EQ(raw.LastReject()->reason, proto::RejectReason::InvalidName);
}

TEST_F(Fixture, MessageBeforeHelloIsProtocolViolation)
{
    StartServer();
    auto& raw = AddRaw();
    Pump();
    SendRaw(raw, proto::Heartbeat{ 1, 1 });
    Pump();
    ASSERT_TRUE(raw.LastReject());
    EXPECT_EQ(raw.LastReject()->reason, proto::RejectReason::ProtocolViolation);
    EXPECT_TRUE(raw.Closed());
}

TEST_F(Fixture, GarbageBytesAreRejected)
{
    StartServer();
    auto& raw = AddRaw();
    Pump();
    const std::vector<std::uint8_t> garbage{ 0xDE, 0xAD, 0xBE, 0xEF, 0x00 };
    raw.transport->Send(raw.connection, garbage, Delivery::Reliable);
    Pump();
    ASSERT_TRUE(raw.LastReject());
    EXPECT_EQ(raw.LastReject()->reason, proto::RejectReason::ProtocolViolation);
    EXPECT_EQ(server->Stats().decodeFailures, 1u);
}

TEST_F(Fixture, OversizeMessageClosesConnection)
{
    StartServer();
    auto& raw = AddRaw();
    Pump();
    std::vector<std::uint8_t> huge(proto::kMaxMessageBytes + 1, 0);
    raw.transport->Send(raw.connection, huge, Delivery::Reliable);
    Pump();
    EXPECT_TRUE(raw.Closed());
    EXPECT_EQ(server->PendingConnectionCount(), 0u);
}

TEST_F(Fixture, SecondHelloAfterRegistrationRemovesPlayer)
{
    StartServer();
    auto& a = AddClient("Alice");
    auto& raw = AddRaw();
    Pump();
    SendRaw(raw, ValidHello("Mallory"));
    Pump();
    ASSERT_EQ(server->ConnectedPlayerCount(), 2u);
    a.Clear();

    SendRaw(raw, ValidHello("Mallory2"));
    Pump();
    EXPECT_TRUE(raw.Closed());
    EXPECT_EQ(server->ConnectedPlayerCount(), 1u);
    const auto left = Collect<client_event::PeerLeft>(a.events);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].reason, proto::LeaveReason::ProtocolViolation);
    EXPECT_EQ(server->Players().size(), 1u);  // no resume slot for violators
}

TEST_F(Fixture, SpoofedTransformIdentityRemovesPlayer)
{
    StartServer();
    AddClient("Alice");
    auto& raw = AddRaw();
    Pump();
    SendRaw(raw, ValidHello("Mallory"));
    Pump();
    proto::PlayerTransform t;
    t.playerId = 1;  // Alice's id
    t.sequence = 1;
    SendRaw(raw, t);
    Pump();
    EXPECT_TRUE(raw.Closed());
    EXPECT_EQ(server->ConnectedPlayerCount(), 1u);
}

TEST_F(Fixture, ServerOnlyMessageFromClientIsViolation)
{
    StartServer();
    auto& raw = AddRaw();
    Pump();
    SendRaw(raw, ValidHello("Mallory"));
    Pump();
    SendRaw(raw, proto::PlayerLeft{ 1, proto::LeaveReason::Kicked });
    Pump();
    EXPECT_TRUE(raw.Closed());
    EXPECT_EQ(server->Stats().protocolViolations, 1u);
}

TEST_F(Fixture, HandshakeTimeout)
{
    StartServer();
    auto& raw = AddRaw();
    Pump();
    EXPECT_EQ(server->PendingConnectionCount(), 1u);
    Advance(6'000);
    ASSERT_TRUE(raw.LastReject());
    EXPECT_EQ(raw.LastReject()->reason, proto::RejectReason::HandshakeTimeout);
    EXPECT_EQ(server->PendingConnectionCount(), 0u);
}

TEST_F(Fixture, PendingConnectionLimit)
{
    ServerConfig cfg;
    cfg.maxPendingConnections = 2;
    StartServer(cfg);
    auto& r1 = AddRaw();
    auto& r2 = AddRaw();
    auto& r3 = AddRaw();
    Pump();
    EXPECT_FALSE(r1.Closed());
    EXPECT_FALSE(r2.Closed());
    EXPECT_TRUE(r3.Closed());
}

TEST_F(Fixture, RateLimitDisconnectsFlooder)
{
    ServerConfig cfg;
    cfg.maxMessagesPerSecond = 10;
    cfg.messageBurst = 20;
    StartServer(cfg);
    auto& raw = AddRaw();
    Pump();
    SendRaw(raw, ValidHello("Flood"));
    for (int i = 0; i < 50; ++i) SendRaw(raw, proto::Heartbeat{ static_cast<std::uint32_t>(i), 0 });
    Pump();
    EXPECT_TRUE(raw.Closed());
    EXPECT_EQ(server->Stats().rateLimitDisconnects, 1u);
    EXPECT_EQ(server->ConnectedPlayerCount(), 0u);
}

TEST_F(Fixture, GracefulDisconnectFreesSlotAndName)
{
    StartServer();
    auto& a = AddClient("Alice");
    auto& b = AddClient("Bob");
    Pump();
    b.Clear();
    a.session->Disconnect();
    Pump();
    const auto left = Collect<client_event::PeerLeft>(b.events);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].reason, proto::LeaveReason::Quit);
    EXPECT_EQ(server->Players().size(), 1u);

    auto& again = AddClient("Alice");
    Pump();
    EXPECT_EQ(again.session->State(), ClientState::InSession);
    EXPECT_EQ(again.session->LocalPlayerId(), 3u);  // ids are never reused
}

TEST_F(Fixture, DroppedConnectionResumesWithSamePlayerId)
{
    StartServer();
    ClientConfig cfg;
    cfg.reconnectDelayMs = 500;
    auto& a = AddClient("Alice", cfg);
    auto& b = AddClient("Bob");
    Pump();
    const auto aliceId = a.session->LocalPlayerId();
    const auto session = a.session->CurrentSessionId();
    b.Clear();
    a.Clear();

    a.transport->SimulateNetworkFailure();
    Pump();
    EXPECT_EQ(a.session->State(), ClientState::Reconnecting);
    ASSERT_EQ(Collect<client_event::PeerLeft>(b.events).size(), 1u);
    EXPECT_EQ(Collect<client_event::PeerLeft>(b.events)[0].reason, proto::LeaveReason::ConnectionLost);
    EXPECT_EQ(server->Players().size(), 2u);  // slot reserved
    EXPECT_EQ(server->ConnectedPlayerCount(), 1u);

    Advance(1'000);
    ASSERT_EQ(a.session->State(), ClientState::InSession);
    EXPECT_EQ(a.session->LocalPlayerId(), aliceId);
    EXPECT_EQ(a.session->CurrentSessionId(), session);
    const auto welcomed = Collect<client_event::Welcomed>(a.events);
    ASSERT_EQ(welcomed.size(), 1u);
    EXPECT_TRUE(welcomed[0].welcome.resumed);
    ASSERT_EQ(Collect<client_event::PeerJoined>(b.events).size(), 1u);
    EXPECT_EQ(Collect<client_event::PeerJoined>(b.events)[0].player.playerId, aliceId);
    EXPECT_EQ(a.session->Roster().size(), 1u);  // still knows Bob
}

TEST_F(Fixture, ResumeAfterWindowExpiryIsAFreshJoin)
{
    ServerConfig scfg;
    scfg.resumeWindowMs = 2'000;
    StartServer(scfg);
    ClientConfig cfg;
    cfg.reconnectDelayMs = 5'000;
    auto& a = AddClient("Alice", cfg);
    Pump();
    const auto first = a.session->LocalPlayerId();
    a.transport->SimulateNetworkFailure();
    Pump();
    Advance(3'000);
    EXPECT_TRUE(server->Players().empty());  // reservation expired

    Advance(3'000);
    ASSERT_EQ(a.session->State(), ClientState::InSession);
    EXPECT_NE(a.session->LocalPlayerId(), first);
}

TEST_F(Fixture, StolenOrRandomResumeTokenDoesNotHijack)
{
    StartServer();
    auto& a = AddClient("Alice");
    auto& raw = AddRaw();
    Pump();
    auto hello = ValidHello("Mallory");
    hello.resumeToken.bytes.fill(0x41);
    SendRaw(raw, hello);
    Pump();
    // Treated as a fresh join under its own name, not as Alice.
    EXPECT_EQ(server->ConnectedPlayerCount(), 2u);
    EXPECT_EQ(a.session->State(), ClientState::InSession);
    const auto players = server->Players();
    EXPECT_EQ(players[0].name, "Alice");
    EXPECT_EQ(players[1].name, "Mallory");
}

TEST_F(Fixture, SilentClientTimesOutAndSlotIsReserved)
{
    StartServer();
    auto& raw = AddRaw();
    auto& b = AddClient("Bob");
    Pump();
    SendRaw(raw, ValidHello("Quiet"));
    Pump();
    b.Clear();
    Advance(11'000);  // raw peer never heartbeats
    EXPECT_TRUE(raw.Closed());
    const auto left = Collect<client_event::PeerLeft>(b.events);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].reason, proto::LeaveReason::ConnectionLost);
}

TEST_F(Fixture, ServerStopNotifiesClients)
{
    StartServer();
    auto& a = AddClient("Alice");
    Pump();
    server->Stop();
    Pump();
    EXPECT_EQ(a.session->State(), ClientState::Disconnected);
    const auto states = Collect<client_event::StateChanged>(a.events);
    ASSERT_FALSE(states.empty());
    EXPECT_NE(states.back().detail.find("shut down"), std::string::npos);
}

TEST_F(Fixture, KickRemovesPlayerWithoutResume)
{
    StartServer();
    auto& a = AddClient("Alice");
    Pump();
    ASSERT_TRUE(server->Kick(a.session->LocalPlayerId(), "bye"));
    Pump();
    EXPECT_EQ(a.session->State(), ClientState::Disconnected);
    EXPECT_TRUE(server->Players().empty());
}

TEST_F(Fixture, ClientReportsConnectionRefused)
{
    // No server listening.
    auto& a = AddClient("Alice");
    Pump();
    EXPECT_EQ(a.session->State(), ClientState::Disconnected);
}

TEST_F(Fixture, ClientRejectsMalformedServer)
{
    LoopbackTransport fakeServer(hub, proto::kMaxMessageBytes);
    std::string error;
    ASSERT_TRUE(fakeServer.Listen(kPort, error));
    auto& a = AddClient("Alice");
    std::vector<TransportEvent> evs;
    Pump();
    fakeServer.Poll(evs);
    ConnectionId conn = kInvalidConnection;
    for (auto& e : evs) {
        if (e.type == TransportEventType::Connected) conn = e.connection;
    }
    ASSERT_NE(conn, kInvalidConnection);
    const std::vector<std::uint8_t> garbage{ 1, 2, 3 };
    fakeServer.Send(conn, garbage, Delivery::Reliable);
    Pump();
    EXPECT_EQ(a.session->State(), ClientState::Disconnected);
}
