#include "cosync/net/LoopbackTransport.h"
#include "cosync/net/SessionClient.h"
#include "cosync/net/SessionServer.h"
#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/NpcReplication.h"
#include "cosync/shared/Presence.h"
#include "cosync/shared/WorldSync.h"

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

// ---- shared world state (docs/WORLD_SYNC.md) ----
namespace
{
    using proto::WorldEvent;
    using proto::WorldEventKind;
    constexpr std::uint32_t kDoor = 0x0012AB34;
    constexpr std::uint32_t kQuest = 0x00020001;

    WorldEvent Door(int open) { return { WorldEventKind::DoorOpen, kDoor, 0x77, open }; }
    WorldEvent Item(int count) { return { WorldEventKind::ContainerItem, kDoor, 0x0F, count }; }
    WorldEvent Stage(int stage) { return { WorldEventKind::QuestStage, kQuest, 0, stage }; }
}

TEST(WorldLedger, NewestDoorReportWinsAndCostsOneEntry)
{
    world::Ledger ledger;
    for (int i = 0; i < 50; ++i) ledger.Apply(Door(i % 2));
    ASSERT_EQ(ledger.Size(), 1u);
    EXPECT_EQ(ledger.Snapshot()[0].value, 1);
}

TEST(WorldLedger, QuestKeepsEveryStageInOrder)
{
    world::Ledger ledger;
    ledger.Apply(Stage(10));
    ledger.Apply(Stage(20));
    ledger.Apply(Stage(10));  // reported by a second player: moves, does not duplicate
    const auto all = ledger.Snapshot();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].value, 20);
    EXPECT_EQ(all[1].value, 10);
}

TEST(WorldLedger, ContainerCountsAddUpAndCancelOut)
{
    world::Ledger ledger;
    ledger.Apply(Item(5));
    ledger.Apply(Item(-2));
    ASSERT_EQ(ledger.Size(), 1u);
    EXPECT_EQ(ledger.Snapshot()[0].value, 3);
    ledger.Apply(Item(-3));
    EXPECT_EQ(ledger.Size(), 0u);
}

TEST(WorldLedger, FullLedgerRefusesNewThingsButStillUpdatesKnownOnes)
{
    world::Ledger ledger;
    for (std::uint32_t i = 1; i <= world::Ledger::kMaxEntries; ++i) {
        ASSERT_TRUE(ledger.Apply({ WorldEventKind::RefRemoved, i, 0, 0 }));
    }
    EXPECT_FALSE(ledger.Apply({ WorldEventKind::RefRemoved, 0xFFFFFF, 0, 0 }));
    EXPECT_TRUE(ledger.Apply({ WorldEventKind::RefRemoved, 1, 0, 0 }));
    EXPECT_EQ(ledger.Size(), world::Ledger::kMaxEntries);
}

TEST(WorldEcho, AppliedChangeIsSwallowedOnceThenForgotten)
{
    world::EchoFilter echo;
    echo.Expect(Door(1), 1000);
    EXPECT_TRUE(echo.Consume(Door(1), 1100));
    EXPECT_FALSE(echo.Consume(Door(1), 1200));  // a second report is a real change
}

TEST(WorldEcho, DifferentChangeIsNotAnEcho)
{
    world::EchoFilter echo;
    echo.Expect(Door(1), 1000);
    EXPECT_FALSE(echo.Consume(Door(0), 1100));
    EXPECT_TRUE(echo.Consume(Door(1), 1100));
}

TEST(WorldEcho, ExpectationRunsOut)
{
    world::EchoFilter echo;
    echo.Expect(Door(1), 1000);
    EXPECT_FALSE(echo.Consume(Door(1), 1000 + world::EchoFilter::kExpiryMs));
}

TEST(WorldClock, StepsOnlyWhenClocksHaveReallyParted)
{
    EXPECT_FALSE(world::NeedsClockStep(10.0f, 10.0f));
    EXPECT_FALSE(world::NeedsClockStep(10.0f, 10.0f + world::kClockToleranceDays / 2.0f));
    EXPECT_TRUE(world::NeedsClockStep(10.0f, 10.5f));
    EXPECT_TRUE(world::NeedsClockStep(10.5f, 10.0f));
    EXPECT_FALSE(world::NeedsClockStep(10.0f, std::nanf("")));
}

TEST(WorldProtocol, EventsRoundTrip)
{
    const proto::WorldEvents in{ 3, { Door(1), Item(-7), Stage(1000), { WorldEventKind::RefRemoved, 0xFF000123, 0x55, 0 } } };
    const auto bytes = proto::Encode(proto::Message{ in });
    const auto out = proto::Decode(bytes);
    ASSERT_TRUE(out.Ok());
    const auto& got = std::get<proto::WorldEvents>(*out.message);
    EXPECT_EQ(got.playerId, 3u);
    EXPECT_EQ(got.events, in.events);
}

TEST(WorldProtocol, ClockRoundTrip)
{
    const proto::WorldClock in{ 1, 123.25f, 6.0f, 0xAB, 0xCD };
    const auto out = proto::Decode(proto::Encode(proto::Message{ in }));
    ASSERT_TRUE(out.Ok());
    const auto& got = std::get<proto::WorldClock>(*out.message);
    EXPECT_EQ(got.gameDaysPassed, in.gameDaysPassed);
    EXPECT_EQ(got.gameHour, in.gameHour);
    EXPECT_EQ(got.weather, in.weather);
}

TEST(WorldProtocol, RefusesNonsense)
{
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::DoorOpen, 0, 0, 1 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::DoorOpen, kDoor, 0, 2 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::QuestStage, kQuest, 0, 70000 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::ContainerItem, kDoor, 0, 1 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::ContainerItem, kDoor, 1, 0 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ static_cast<WorldEventKind>(99), kDoor, 0, 0 }));
    auto bad = proto::Encode(proto::Message{ proto::WorldClock{ 1, 1.0f, 99.0f, 0, 0 } });
    EXPECT_FALSE(proto::Decode(bad).Ok());
}

TEST_F(Session, WorldEventFromAnyPlayerReachesTheOthers)
{
    auto& host = Add("Host");
    auto& guest = Add("Guest");
    ASSERT_TRUE(guest.session->SendWorldEvents({ Door(1) }));
    Pump();
    const auto got = host.Got<client_event::WorldEvents>();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].world.playerId, guest.session->LocalPlayerId());
    EXPECT_EQ(got[0].world.events, std::vector<WorldEvent>{ Door(1) });
    EXPECT_TRUE(guest.Got<client_event::WorldEvents>().empty());  // never echoed to its sender
}

TEST_F(Session, LateJoinerGetsTheWorldAsItIsNow)
{
    auto& host = Add("Host");
    host.session->SendWorldEvents({ Door(1), Stage(10), Item(5) });
    host.session->SendWorldEvents({ Door(0), Stage(20), Item(-2) });
    host.session->SendWorldClock({ 0, 50.5f, 12.0f, 0x10, 0x20 });
    Pump();
    auto& late = Add("Late");
    Pump();
    std::vector<WorldEvent> seen;
    for (const auto& m : late.Got<client_event::WorldEvents>()) seen.insert(seen.end(), m.world.events.begin(), m.world.events.end());
    ASSERT_EQ(seen.size(), 4u);  // the door once, both stages, the item once
    const auto has = [&](const WorldEvent& e) { return std::ranges::find(seen, e) != seen.end(); };
    EXPECT_TRUE(has(Door(0)));
    EXPECT_TRUE(has(Stage(10)));
    EXPECT_TRUE(has(Stage(20)));
    EXPECT_TRUE(has(Item(3)));
    const auto clocks = late.Got<client_event::WorldClock>();
    ASSERT_EQ(clocks.size(), 1u);
    EXPECT_EQ(clocks[0].clock.gameDaysPassed, 50.5f);
}

TEST_F(Session, OnlyTheAuthorityRunsTheClock)
{
    Add("Host");
    auto& rogue = Add("Rogue");
    auto& witness = Add("Witness");
    rogue.session->SendWorldClock({ 0, 1.0f, 1.0f, 0, 0 });
    Pump();
    EXPECT_TRUE(witness.Got<client_event::WorldClock>().empty());
    EXPECT_EQ(server->Stats().npcMessagesRefused, 1u);
    EXPECT_EQ(server->ConnectedPlayerCount(), 3u);
}

TEST(WorldContainers, OnlyDataFileContainersAreShared)
{
    EXPECT_TRUE(world::CanShareContainer(0x0022ED8A));
    EXPECT_FALSE(world::CanShareContainer(0xFF010C54));  // numbered per copy
    EXPECT_FALSE(world::CanShareContainer(0));
}

TEST(WorldLocks, NewestLockStateWinsAndReachesLateJoiners)
{
    world::Ledger ledger;
    ledger.Apply({ WorldEventKind::LockState, 0x001661A7, 0x55, 0 });
    ledger.Apply({ WorldEventKind::LockState, 0x001661A7, 0x55, 1 });
    const auto snapshot = ledger.Snapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0].value, 1);
}

TEST(WorldLocks, ProtocolAcceptsOnlyLockedOrUnlocked)
{
    EXPECT_TRUE(proto::IsValidWorldEvent({ WorldEventKind::LockState, 0x001661A7, 0x55, 0 }));
    EXPECT_TRUE(proto::IsValidWorldEvent({ WorldEventKind::LockState, 0x001661A7, 0x55, 1 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::LockState, 0x001661A7, 0x55, 2 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::Destroyed, 0x001661A7, 0x55, -1 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ static_cast<WorldEventKind>(200), 0x001661A7, 0x55, 1 }));
}

TEST(Presence, KnownApartOnlyWhenTheyReallyAre)
{
    using presence::Place;
    constexpr std::uint32_t kOutside = 0x80001234, kOutsideNext = 0x80001235, kInside = 0x00005678;
    EXPECT_TRUE(presence::MayShare(Place{ 0, {} }, Place{ kInside, {} }));  // not known yet
    EXPECT_TRUE(presence::MayShare(Place{ kInside, {} }, Place{ kInside, { 900, 0, 0 } }));
    EXPECT_FALSE(presence::MayShare(Place{ kInside, {} }, Place{ 0x00005679, {} }));
    EXPECT_FALSE(presence::MayShare(Place{ kInside, {} }, Place{ kOutside, {} }));
    EXPECT_TRUE(presence::MayShare(Place{ kOutside, { 0, 0, 0 } }, Place{ kOutsideNext, { 3000, 0, 0 } }));  // next cell over
    EXPECT_FALSE(presence::MayShare(Place{ kOutside, { 0, 0, 0 } }, Place{ kOutsideNext, { 20000, 0, 0 } }));
}

TEST_F(Session, NpcStatesGoOnlyToPlayersWhoCanSeeThem)
{
    auto& host = Add("Host");
    auto& near = Add("Near");
    auto& away = Add("Away");
    host.session->SendTransform(0x00005678, { 0, 0, 0 }, { 0, 0, 0 });
    near.session->SendTransform(0x00005678, { 50, 0, 0 }, { 0, 0, 0 });
    away.session->SendTransform(0x00009999, { 0, 0, 0 }, { 0, 0, 0 });
    Pump();
    ASSERT_TRUE(host.session->SendNpcStates(0x00005678, { At(kNpc, 5.0f) }));
    Pump();
    EXPECT_EQ(near.Got<client_event::NpcStates>().size(), 1u);
    EXPECT_TRUE(away.Got<client_event::NpcStates>().empty());
    EXPECT_EQ(server->Stats().npcMessagesNotRelevant, 1u);
    // A death still reaches everyone.
    ASSERT_TRUE(host.session->SendNpcDeath(kNpc));
    Pump();
    EXPECT_FALSE(away.Got<client_event::NpcDeath>().empty());
}

TEST_F(Session, DialogueReachesOnlyPlayersNearby)
{
    auto& talker = Add("Talker");
    auto& near = Add("Near");
    auto& away = Add("Away");
    talker.session->SendTransform(0x00005678, { 0, 0, 0 }, { 0, 0, 0 });
    near.session->SendTransform(0x00005678, { 50, 0, 0 }, { 0, 0, 0 });
    away.session->SendTransform(0x00009999, { 0, 0, 0 }, { 0, 0, 0 });
    Pump();
    ASSERT_TRUE(talker.session->SendDialogueLine({ 0, "UC Security", "Move along.", false }));
    Pump();
    const auto heard = near.Got<client_event::DialogueLine>();
    ASSERT_EQ(heard.size(), 1u);
    EXPECT_EQ(heard[0].line.playerId, talker.session->LocalPlayerId());
    EXPECT_EQ(heard[0].line.text, "Move along.");
    EXPECT_TRUE(away.Got<client_event::DialogueLine>().empty());
    EXPECT_TRUE(talker.Got<client_event::DialogueLine>().empty());
}
TEST(WorldPickups, FirstTakerKeepsIt)
{
    world::Pickups pickups;
    EXPECT_TRUE(pickups.Claim(0x1234, 1));
    EXPECT_TRUE(pickups.Claim(0x1234, 1));   // the same player again
    EXPECT_FALSE(pickups.Claim(0x1234, 2));  // someone else, later
    EXPECT_TRUE(pickups.Claim(0x5678, 2));
}

TEST(WorldProtocol, ItemReturnedNeedsAnItemAndACount)
{
    EXPECT_TRUE(proto::IsValidWorldEvent({ WorldEventKind::ItemReturned, 0x1234, 0x55, 3 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::ItemReturned, 0x1234, 0, 3 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::ItemReturned, 0x1234, 0x55, 0 }));
    EXPECT_TRUE(proto::IsValidWorldEvent({ WorldEventKind::RefRemoved, 0x1234, 0x55, 12 }));
    EXPECT_FALSE(proto::IsValidWorldEvent({ WorldEventKind::RefRemoved, 0x1234, 0x55, -1 }));
    world::Ledger ledger;
    EXPECT_TRUE(ledger.Apply({ WorldEventKind::ItemReturned, 0x1234, 0x55, 3 }));
    EXPECT_EQ(ledger.Size(), 0u);  // an answer, not world state
}

TEST_F(Session, SecondPlayerToPickUpAnItemGivesItBack)
{
    auto& first = Add("First");
    auto& second = Add("Second");
    auto& watcher = Add("Watcher");
    Pump();
    ASSERT_TRUE(first.session->SendWorldEvents({ { WorldEventKind::RefRemoved, 0x0001A2B3, 0x55, 2 } }));
    Pump();
    ASSERT_TRUE(second.session->SendWorldEvents({ { WorldEventKind::RefRemoved, 0x0001A2B3, 0x55, 2 } }));
    Pump();
    const auto back = second.Got<client_event::WorldEvents>();
    std::vector<proto::WorldEvent> returned;
    for (const auto& m : back) {
        for (const auto& e : m.world.events) {
            if (e.kind == WorldEventKind::ItemReturned) returned.push_back(e);
        }
    }
    ASSERT_EQ(returned.size(), 1u);
    EXPECT_EQ(returned[0].formId, 0x0001A2B3u);
    EXPECT_EQ(returned[0].itemId, 0x55u);
    EXPECT_EQ(returned[0].value, 2);
    std::size_t removals = 0;
    for (const auto& m : watcher.Got<client_event::WorldEvents>()) {
        for (const auto& e : m.world.events) removals += e.kind == WorldEventKind::RefRemoved ? 1 : 0;
        for (const auto& e : m.world.events) EXPECT_NE(e.kind, WorldEventKind::ItemReturned);
    }
    EXPECT_EQ(removals, 1u);  // only the first taker's
    EXPECT_EQ(server->Stats().itemsReturned, 1u);
}

TEST(WorldProtocol, DroppedItemRoundTripAndValidation)
{
    const proto::DroppedItem drop{ 2, proto::MakeSharedId(2, 7), 0x00066800, 3, 0x00005678, { 1.5f, -2.0f, 30.0f } };
    EXPECT_EQ(proto::SharedIdOwner(drop.sharedId), 2u);
    const auto decoded = proto::Decode(proto::Encode(drop));
    ASSERT_TRUE(decoded.Ok());
    const auto& d = std::get<proto::DroppedItem>(*decoded.message);
    EXPECT_EQ(d.sharedId, drop.sharedId);
    EXPECT_EQ(d.baseId, 0x00066800u);
    EXPECT_EQ(d.count, 3);
    EXPECT_EQ(d.locationId, 0x00005678u);
    EXPECT_FLOAT_EQ(d.position.y, -2.0f);
    auto bad = drop;
    bad.baseId = 0xFF001234;  // runtime forms mean nothing on another copy
    EXPECT_FALSE(proto::IsValidDroppedItem(bad));
    bad = drop;
    bad.count = 0;
    EXPECT_FALSE(proto::IsValidDroppedItem(bad));
    bad = drop;
    bad.sharedId = proto::MakeSharedId(2, 0);
    EXPECT_FALSE(proto::IsValidDroppedItem(bad));
    EXPECT_TRUE(proto::IsValidWorldEvent({ WorldEventKind::SharedRemoved, drop.sharedId, 0x00066800, 3 }));
}

TEST_F(Session, DropReachesOthersAndLateJoinersUntilPickedUp)
{
    auto& dropper = Add("Dropper");
    auto& other = Add("Other");
    Pump();
    const auto id = proto::MakeSharedId(dropper.session->LocalPlayerId(), 1);
    ASSERT_TRUE(dropper.session->SendDroppedItem({ 0, id, 0x00066800, 2, 0x00005678, { 1, 2, 3 } }));
    Pump();
    const auto got = other.Got<client_event::DroppedItem>();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].drop.playerId, dropper.session->LocalPlayerId());
    auto& late = Add("Late");
    Pump();
    EXPECT_EQ(late.Got<client_event::DroppedItem>().size(), 1u);
    // Picked up, then nobody joining later sees it.
    ASSERT_TRUE(other.session->SendWorldEvents({ { WorldEventKind::SharedRemoved, id, 0x00066800, 2 } }));
    Pump();
    auto& later = Add("Later");
    Pump();
    EXPECT_TRUE(later.Got<client_event::DroppedItem>().empty());
    EXPECT_EQ(server->Stats().dropsRelayed, 1u);
}

TEST_F(Session, DropUnderSomeoneElsesIdIsRefused)
{
    auto& dropper = Add("Dropper");
    auto& other = Add("Other");
    Pump();
    const auto foreign = proto::MakeSharedId(other.session->LocalPlayerId(), 1);
    ASSERT_TRUE(dropper.session->SendDroppedItem({ 0, foreign, 0x00066800, 1, 0x00005678, { 1, 2, 3 } }));
    Pump();
    EXPECT_TRUE(other.Got<client_event::DroppedItem>().empty());
    EXPECT_EQ(server->Stats().protocolViolations, 1u);
}

TEST(WorldLedger, ResumedCopyAppliesOnlyTheContainerChangesItMissed)
{
    world::Ledger seen;  // what this copy applied before the connection dropped
    ASSERT_TRUE(seen.Apply({ WorldEventKind::ContainerItem, 0x0022ED8A, 0x55, -3 }));
    const proto::WorldEvent history{ WorldEventKind::ContainerItem, 0x0022ED8A, 0x55, -5 };  // the session's total
    EXPECT_EQ(world::MissingContainerChange(seen, history), -2);
    EXPECT_EQ(world::MissingContainerChange(world::Ledger{}, history), -5);  // a fresh join takes all of it
    ASSERT_TRUE(seen.Apply({ WorldEventKind::ContainerItem, 0x0022ED8A, 0x55, -2 }));
    EXPECT_EQ(world::MissingContainerChange(seen, history), 0);
}
