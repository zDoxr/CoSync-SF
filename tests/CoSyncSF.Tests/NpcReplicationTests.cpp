#include "cosync/net/LoopbackTransport.h"
#include "cosync/net/SessionClient.h"
#include "cosync/net/SessionServer.h"
#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/NpcReplication.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

using namespace cosync;
using namespace cosync::net;

namespace
{
    constexpr std::uint16_t kPort = 7778;
    constexpr std::uint32_t kNpc = 0x00015062;

    proto::NpcState At(std::uint32_t id, float x)
    {
        proto::NpcState s;
        s.formId = id;
        s.position = { x, 0.0f, 0.0f };
        return s;
    }

    // ---- server + clients over the in-memory transport ----
    struct Session : ::testing::Test
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
            std::vector<ClientEvent> events;

            template <class T>
            std::vector<T> Got() const
            {
                std::vector<T> out;
                for (const auto& e : events) {
                    if (auto* p = std::get_if<T>(&e)) out.push_back(*p);
                }
                return out;
            }
        };
        std::vector<std::unique_ptr<Client>> clients;

        void SetUp() override
        {
            serverTransport = std::make_unique<LoopbackTransport>(hub, proto::kMaxMessageBytes);
            server = std::make_unique<SessionServer>(*serverTransport, ServerConfig{}, clock);
            std::string error;
            ASSERT_TRUE(server->Start(kPort, error)) << error;
        }

        Client& Add(std::string name, std::uint32_t runtime = game::kTargetRuntime)
        {
            auto c = std::make_unique<Client>();
            c->transport = std::make_unique<LoopbackTransport>(hub, proto::kMaxMessageBytes);
            ClientConfig config;
            config.playerName = std::move(name);
            config.gameRuntime = runtime;
            c->session = std::make_unique<SessionClient>(*c->transport, config, clock);
            std::string error;
            EXPECT_TRUE(c->session->Connect({ "127.0.0.1", kPort }, error)) << error;
            clients.push_back(std::move(c));
            Pump();
            return *clients.back();
        }

        void Pump(int rounds = 6)
        {
            for (int i = 0; i < rounds; ++i) {
                server->Update();
                for (auto& c : clients) c->session->Update(c->events);
            }
        }
    };
}

// ---- protocol ----

TEST(NpcProtocol, CarriesRuntimeReferencesWithTheirBase)
{
    auto s = At(0xFF001234, 1.0f);
    s.baseId = 0xFF000111;
    const auto d = proto::Decode(proto::Encode(proto::NpcStates{ 0, 1, 2, { s } }));
    ASSERT_TRUE(d.Ok());
    const auto& got = std::get<proto::NpcStates>(*d.message).npcs.at(0);
    EXPECT_EQ(got.formId, 0xFF001234u);
    EXPECT_EQ(got.baseId, 0xFF000111u);
}

TEST(NpcProtocol, CarriesTheCrowdTargetOnlyWhenFlagged)
{
    auto s = At(kNpc, 1.0f);
    const auto plain = proto::Encode(proto::NpcStates{ 0, 1, 2, { s } });
    s.flags = proto::kNpcCrowdTarget;
    s.crowdTarget = { 10.0f, -20.0f, 30.0f };
    const auto flagged = proto::Encode(proto::NpcStates{ 0, 1, 2, { s } });
    EXPECT_EQ(flagged.size(), plain.size() + 3 * sizeof(float));
    const auto d = proto::Decode(flagged);
    ASSERT_TRUE(d.Ok());
    const auto& got = std::get<proto::NpcStates>(*d.message).npcs.at(0);
    EXPECT_EQ(got.flags, proto::kNpcCrowdTarget);
    EXPECT_EQ(got.crowdTarget.y, -20.0f);
}

TEST(CrowdFollow, HoldsOnlyWhereTheEngineWouldDecide)
{
    using npc::CrowdStep;
    for (const std::uint32_t state : { 3u, 4u, 6u, 7u, 8u, 9u, 10u }) {
        EXPECT_EQ(npc::DecideCrowdStep(state, true), CrowdStep::Hold) << state;
    }
    for (const std::uint32_t state : { 0u, 1u, 2u, 5u, 11u, 12u, 14u, 15u, 16u, 17u, 23u, 24u, 25u }) {
        EXPECT_EQ(npc::DecideCrowdStep(state, true), CrowdStep::Run) << state;
    }
}

TEST(CrowdFollow, RedirectsUnlessTheActorIsCommitted)
{
    using npc::CrowdStep;
    for (std::uint32_t state = 0; state <= 12; ++state) {
        EXPECT_EQ(npc::DecideCrowdStep(state, false), CrowdStep::Retarget) << state;
    }
    EXPECT_EQ(npc::DecideCrowdStep(24, false), CrowdStep::Retarget);
    for (const std::uint32_t state : { 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 25u, 40u }) {
        EXPECT_EQ(npc::DecideCrowdStep(state, false), CrowdStep::Run) << state;
    }
}

TEST(CrowdFollow, MatchesTargetsByPositionOnly)
{
    const std::vector<npc::CrowdTarget> targets{ { 11, { 0.0f, 0.0f, 0.0f } }, { 22, { 5.0f, 0.0f, 0.0f } }, { 33, { 5.1f, 0.0f, 0.0f } } };
    EXPECT_EQ(npc::FindCrowdTarget(targets, { 5.08f, 0.0f, 0.0f }), 33u);
    EXPECT_EQ(npc::FindCrowdTarget(targets, { 0.01f, 0.0f, 0.0f }), 11u);
    EXPECT_EQ(npc::FindCrowdTarget(targets, { 2.5f, 0.0f, 0.0f }), 0u);
    EXPECT_EQ(npc::FindCrowdTarget({}, { 0.0f, 0.0f, 0.0f }), 0u);
}

TEST(NpcProtocol, SplitForWireKeepsEveryNpcInOrder)
{
    std::vector<proto::NpcState> npcs;
    for (std::uint32_t i = 1; i <= 2 * proto::kMaxNpcStates + 3; ++i) npcs.push_back(At(i, 0.0f));
    const auto parts = npc::SplitForWire(npcs);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0].size(), proto::kMaxNpcStates);
    EXPECT_EQ(parts[2].size(), 3u);
    std::uint32_t next = 1;
    for (const auto& part : parts) {
        for (const auto& n : part) EXPECT_EQ(n.formId, next++);
    }
    EXPECT_TRUE(npc::SplitForWire({}).empty());
}

TEST(NpcProtocol, RejectsBadValues)
{
    using namespace proto;
    const auto states = [](NpcState s) { return Decode(Encode(NpcStates{ 0, 1, 2, { s } })).Ok(); };
    EXPECT_TRUE(states(At(kNpc, 1.0f)));
    EXPECT_FALSE(states(At(0, 1.0f)));
    EXPECT_FALSE(states(At(kNpc, std::nanf(""))));
    EXPECT_FALSE(states(At(kNpc, kMaxCoordinate * 2.0f)));
    auto s = At(kNpc, 1.0f);
    s.healthPercent = 101;
    EXPECT_FALSE(states(s));
    s = At(kNpc, 1.0f);
    s.flags = kNpcCrowdTarget;
    s.crowdTarget = { std::nanf(""), 0.0f, 0.0f };
    EXPECT_FALSE(states(s));
    s.crowdTarget = { kMaxCoordinate * 2.0f, 0.0f, 0.0f };
    EXPECT_FALSE(states(s));

    EXPECT_FALSE(Decode(Encode(NpcDeath{ 0, 0 })).Ok());
    EXPECT_FALSE(Decode(Encode(NpcDamage{ 0, kNpc, 0.0f })).Ok());
    EXPECT_FALSE(Decode(Encode(NpcDamage{ 0, kNpc, -5.0f })).Ok());
    EXPECT_FALSE(Decode(Encode(NpcDamage{ 0, kNpc, kMaxNpcDamage * 2.0f })).Ok());
}

TEST(NpcProtocol, FullSnapshotPartFitsTheMessageLimit)
{
    proto::NpcStates m;
    for (std::uint32_t i = 0; i < proto::kMaxNpcStates; ++i) {
        m.npcs.push_back(At(kNpc + i, 1.0f));
        m.npcs.back().flags = proto::kNpcCrowdTarget;
    }
    const auto bytes = proto::Encode(m);
    EXPECT_LT(bytes.size(), proto::kMaxMessageBytes);
    const auto d = proto::Decode(bytes);
    ASSERT_TRUE(d.Ok());
    EXPECT_EQ(std::get<proto::NpcStates>(*d.message).npcs.size(), proto::kMaxNpcStates);
}

// ---- authority ----

TEST_F(Session, FirstGameClientIsAuthorityAndEveryoneIsTold)
{
    auto& host = Add("Host");
    auto& joiner = Add("Joiner");
    const auto id = host.session->LocalPlayerId();
    EXPECT_EQ(server->NpcAuthority(), id);
    EXPECT_EQ(host.session->NpcAuthority(), id);
    EXPECT_EQ(joiner.session->NpcAuthority(), id);
}

TEST_F(Session, ToolClientsNeverBecomeAuthority)
{
    Add("Tool", 0);
    EXPECT_EQ(server->NpcAuthority(), kInvalidPlayerId);
    auto& game = Add("Game");
    EXPECT_EQ(server->NpcAuthority(), game.session->LocalPlayerId());
}

TEST_F(Session, AuthorityMovesWhenItsOwnerLeaves)
{
    auto& host = Add("Host");
    auto& other = Add("Other");
    auto& third = Add("Third");
    host.session->Disconnect();
    Pump();
    EXPECT_EQ(server->NpcAuthority(), other.session->LocalPlayerId());
    EXPECT_EQ(third.session->NpcAuthority(), other.session->LocalPlayerId());
    ASSERT_FALSE(other.Got<client_event::NpcAuthority>().empty());
    EXPECT_EQ(other.Got<client_event::NpcAuthority>().back().authority, other.session->LocalPlayerId());
}

TEST_F(Session, StatesFromAuthorityReachOthersOnly)
{
    auto& host = Add("Host");
    auto& joiner = Add("Joiner");
    ASSERT_TRUE(host.session->SendNpcStates(0x1234, { At(kNpc, 5.0f) }));
    Pump();
    const auto got = joiner.Got<client_event::NpcStates>();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].states.playerId, host.session->LocalPlayerId());
    EXPECT_EQ(got[0].states.locationId, 0x1234u);
    ASSERT_EQ(got[0].states.npcs.size(), 1u);
    EXPECT_EQ(got[0].states.npcs[0].formId, kNpc);
    EXPECT_TRUE(host.Got<client_event::NpcStates>().empty());
}

TEST_F(Session, NonAuthorityCannotPublishStatesOrDeaths)
{
    Add("Host");
    auto& rogue = Add("Rogue");
    auto& witness = Add("Witness");
    rogue.session->SendNpcStates(0x1234, { At(kNpc, 5.0f) });
    rogue.session->SendNpcDeath(kNpc);
    Pump();
    EXPECT_TRUE(witness.Got<client_event::NpcStates>().empty());
    EXPECT_TRUE(witness.Got<client_event::NpcDeath>().empty());
    EXPECT_EQ(server->Stats().npcMessagesRefused, 2u);
    EXPECT_EQ(server->ConnectedPlayerCount(), 3u);  // a late message after losing the role is no offence
}

TEST(NpcOutfits, DiffSaysWhatToEquipAndTakeOff)
{
    const auto c = npc::DiffOutfit({ 1, 2, 5 }, { 2, 3, 4 });
    EXPECT_EQ(c.equip, (std::vector<std::uint32_t>{ 1, 5 }));
    EXPECT_EQ(c.unequip, (std::vector<std::uint32_t>{ 3, 4 }));
    EXPECT_TRUE(npc::DiffOutfit({ 7 }, { 7 }).Empty());
}

TEST(NpcOutfits, SplitKeepsEveryNpcInOrder)
{
    std::vector<proto::NpcOutfit> all;
    const auto total = 2 * proto::kMaxNpcOutfits + 3;
    for (std::uint32_t i = 1; i <= total; ++i) all.push_back({ i, { i + 1000 } });
    const auto parts = npc::SplitOutfits(all);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0].size(), proto::kMaxNpcOutfits);
    EXPECT_EQ(parts[2].size(), 3u);
    EXPECT_EQ(parts[2][2].formId, total);
}

// A full snapshot, its largest case, has to fit under what the server allows next to position
// updates: losing the connection is far worse than a coarser snapshot.
TEST(NpcBudget, MaxPublishedSnapshotStaysUnderTheServerRate)
{
    constexpr double kPositionHz = 60.0, kSnapshotHz = 10.0, kEventHz = 10.0;
    constexpr double kMaxPublished = 300.0;
    const double parts = std::ceil(kMaxPublished / static_cast<double>(proto::kMaxNpcStates));
    const double outfits = std::ceil(kMaxPublished / static_cast<double>(proto::kMaxNpcOutfits));
    EXPECT_LT(kPositionHz + kSnapshotHz * parts + kEventHz + outfits, ServerConfig{}.maxMessagesPerSecond * 0.6);
}

TEST_F(Session, ActionsAndEquipmentComeFromTheAuthorityOnly)
{
    auto& host = Add("Host");
    auto& rogue = Add("Rogue");
    auto& witness = Add("Witness");
    ASSERT_TRUE(host.session->SendNpcEvents({ { kNpc, "weaponDraw" } }));
    ASSERT_TRUE(host.session->SendNpcOutfits({ { kNpc, { 0x1234 } } }));
    rogue.session->SendNpcEvents({ { kNpc, "fake" } });
    rogue.session->SendNpcOutfits({ { kNpc, { 0x9999 } } });
    Pump();
    const auto events = witness.Got<client_event::NpcEvents>();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].events.events.at(0).name, "weaponDraw");
    const auto outfits = witness.Got<client_event::NpcOutfits>();
    ASSERT_EQ(outfits.size(), 1u);
    EXPECT_EQ(outfits[0].outfits.outfits.at(0).items.at(0), 0x1234u);
    EXPECT_EQ(server->Stats().npcMessagesRefused, 2u);
}

TEST_F(Session, DamageGoesToTheAuthorityOnlyAndIsStamped)
{
    auto& host = Add("Host");
    auto& shooter = Add("Shooter");
    auto& witness = Add("Witness");
    ASSERT_TRUE(shooter.session->SendNpcDamage(kNpc, 25.0f));
    Pump();
    const auto got = host.Got<client_event::NpcDamage>();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].damage.playerId, shooter.session->LocalPlayerId());
    EXPECT_FLOAT_EQ(got[0].damage.amount, 25.0f);
    EXPECT_TRUE(witness.Got<client_event::NpcDamage>().empty());
}

TEST_F(Session, LateJoinerHearsEarlierDeathsOnce)
{
    auto& host = Add("Host");
    Add("Early");
    host.session->SendNpcDeath(kNpc);
    host.session->SendNpcDeath(kNpc);  // repeated announcement
    Pump();
    auto& late = Add("Late");
    const auto deaths = late.Got<client_event::NpcDeath>();
    ASSERT_EQ(deaths.size(), 1u);
    EXPECT_EQ(deaths[0].death.npcFormId, kNpc);
}

TEST_F(Session, NewAuthorityStartsItsOwnSequence)
{
    auto& host = Add("Host");
    auto& next = Add("Next");
    auto& witness = Add("Witness");
    host.session->SendNpcStates(1, { At(kNpc, 1.0f) });
    host.session->SendNpcStates(1, { At(kNpc, 2.0f) });
    Pump();
    ASSERT_EQ(witness.Got<client_event::NpcStates>().size(), 2u);
    host.session->Disconnect();
    Pump();
    witness.events.clear();
    // Sequence 1 again, but from a new sender: not stale.
    ASSERT_TRUE(next.session->SendNpcStates(1, { At(kNpc, 3.0f) }));
    Pump();
    EXPECT_EQ(witness.Got<client_event::NpcStates>().size(), 1u);
}

// ---- decisions ----

TEST(NpcDecisions, KeepNearestSortsAndCuts)
{
    std::vector<proto::NpcState> npcs{ At(1, 30.0f), At(2, 10.0f), At(3, 20.0f) };
    npc::KeepNearest(npcs, { 0.0f, 0.0f, 0.0f }, 2);
    ASSERT_EQ(npcs.size(), 2u);
    EXPECT_EQ(npcs[0].formId, 2u);
    EXPECT_EQ(npcs[1].formId, 3u);
}

TEST(NpcDecisions, SteerStandsStillOnArrival)
{
    const auto s = npc::Steer({ 0.1f, 0.1f, 0.0f }, 0.0f, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(s.magnitude, 0.0f);
}

TEST(NpcDecisions, SteerMatchesEngineAxes)
{
    // Yaw 0 faces +Y (forward = (sin yaw, cos yaw)).
    EXPECT_NEAR(npc::Steer({ 0.0f, 2.0f, 0.0f }, 0.0f, 0.0f, 0.0f).angle, 0.0f, 1e-5f);  // ahead
    const float half = std::numbers::pi_v<float> / 2.0f;
    EXPECT_NEAR(npc::Steer({ 2.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f).angle, half, 1e-5f);    // to the right
    // Facing +X, a target ahead on +X needs no sideways input.
    EXPECT_NEAR(npc::Steer({ 2.0f, 0.0f, 0.0f }, 0.0f, 0.0f, half).angle, 0.0f, 1e-5f);
}

TEST(NpcDecisions, SteerCapsAtRun)
{
    EXPECT_FLOAT_EQ(npc::Steer({ 100.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f).magnitude, 2.0f);
}

TEST(NpcDecisions, LocalHitIsForwardedOnce)
{
    npc::HealthTracker h;
    EXPECT_FLOAT_EQ(h.ObserveLocal(80.0f), 0.0f);  // first sight
    EXPECT_FALSE(h.OnAuthoritative(80.0f, 80.0f).has_value());
    EXPECT_FLOAT_EQ(h.ObserveLocal(60.0f), 20.0f);  // this client's player hit it
    EXPECT_FLOAT_EQ(h.ObserveLocal(60.0f), 0.0f);   // not again
}

TEST(NpcDecisions, AuthorityValueIsNotReportedBackAsAHit)
{
    npc::HealthTracker h;
    h.ObserveLocal(80.0f);
    h.OnAuthoritative(80.0f, 80.0f);
    const auto change = h.OnAuthoritative(50.0f, 80.0f);  // another player's hit, via the authority
    ASSERT_TRUE(change.has_value());
    EXPECT_FLOAT_EQ(*change, -30.0f);
    EXPECT_FLOAT_EQ(h.ObserveLocal(50.0f), 0.0f);  // our own write of that value is no hit
}

TEST(NpcDecisions, RacingHitIsNotCountedTwice)
{
    npc::HealthTracker h;
    h.ObserveLocal(80.0f);
    h.OnAuthoritative(80.0f, 80.0f);
    EXPECT_FLOAT_EQ(h.ObserveLocal(70.0f), 10.0f);  // hit here; the snapshot in flight still says 80
    const auto heal = h.OnAuthoritative(80.0f, 70.0f);
    ASSERT_TRUE(heal.has_value());
    EXPECT_FLOAT_EQ(*heal, 10.0f);
    EXPECT_FLOAT_EQ(h.ObserveLocal(80.0f), 0.0f);
    const auto after = h.OnAuthoritative(70.0f, 80.0f);  // the authority applied it
    ASSERT_TRUE(after.has_value());
    EXPECT_FLOAT_EQ(*after, -10.0f);
    EXPECT_FLOAT_EQ(h.ObserveLocal(70.0f), 0.0f);
}

TEST(NpcDecisions, OnlyTheDeathEventKills)
{
    npc::HealthTracker h;
    h.ObserveLocal(10.0f);
    h.OnAuthoritative(10.0f, 10.0f);
    const auto change = h.OnAuthoritative(0.0f, 10.0f);
    ASSERT_TRUE(change.has_value());
    EXPECT_FLOAT_EQ(*change, -9.0f);  // down to 1%, never to 0
}
