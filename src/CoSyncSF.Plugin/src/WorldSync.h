#pragma once

#include "cosync/net/NetworkWorker.h"
#include "cosync/shared/WorldSync.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Shared world state: doors, things taken from the world, the clock and the weather
// (docs/WORLD_SYNC.md). Main thread only, except the event sink, which only queues a form ID.
namespace cosync::plugin::worldsync
{
    class Replication
    {
    public:
        struct Context
        {
            bool ready = false;        // player in the world, VM settled, in session
            PlayerId localPlayer = kInvalidPlayerId;
            std::uint32_t locationId = 0;
        };

        void Tick(net::NetworkWorker& worker, const Context& context);
        void OnEvents(const proto::WorldEvents& events);
        void OnClock(const proto::WorldClock& clock);
        void OnDrop(const proto::DroppedItem& drop);
        void OnAuthority(PlayerId authority) noexcept { _authority = authority; }
        void Reset(const char* why);

        // '@door' and '@item': the local player activates the nearest one (DevHarness).
        bool RunStep(const std::string& step);
        void SetWorker(net::NetworkWorker* worker) noexcept { _worker = worker; }
        // How runtime NPCs are named across copies (NPC sync's pairing). Without it their loot stays local.
        using IdMap = std::function<std::uint32_t(std::uint32_t)>;
        void SetNpcIds(IdMap toShared, IdMap toLocal) { _npcToShared = std::move(toShared); _npcToLocal = std::move(toLocal); }

    private:
        using Clock = std::chrono::steady_clock;

        // What the local player touched, until the engine shows what became of it.
        struct Watch
        {
            std::uint32_t ref = 0;
            std::uint32_t base = 0;
            std::uint8_t doorStateBefore = 0;  // 0 = not a door
            Clock::time_point until{};
            Clock::time_point goneAt{};        // seen gone, waiting a moment for the inventory event
        };
        // Read back from the engine after applying, so a log shows what really happened.
        struct Verify
        {
            proto::WorldEvent event;
            Clock::time_point at{};
        };

        struct Incoming
        {
            proto::WorldEvent event;
            bool mayStart = false;  // may start a quest that is not running here
        };

        void DrainActivations(Clock::time_point now);
        void DetectDrops(net::NetworkWorker& worker, const Context& context, Clock::time_point now);
        void PlaceDrops(const Context& context, Clock::time_point now);
        void ForgetDrop(std::uint32_t sharedId);
        void ReplayAfterLoad();
        void UpdateWatches(net::NetworkWorker& worker, Clock::time_point now);
        void ApplyPending(Clock::time_point now);
        void RunVerifies(Clock::time_point now);
        void PublishClock(net::NetworkWorker& worker, const Context& context, Clock::time_point now);
        void ApplyClock(const Context& context, Clock::time_point now);
        // False to try again later: the thing is not somewhere it can be changed yet.
        [[nodiscard]] bool Apply(const proto::WorldEvent& event, bool mayStart, Clock::time_point now);

        PlayerId _authority = kInvalidPlayerId;
        net::NetworkWorker* _worker = nullptr;
        IdMap _npcToShared, _npcToLocal;
        std::uint64_t _npcLoot = 0;  // for '@netdrop'
        bool _sinkInstalled = false;
        world::EchoFilter _echo;
        std::vector<Watch> _watches;
        std::vector<proto::WorldEvent> _outgoing;
        std::deque<Incoming> _pending;
        std::vector<Verify> _verifies;
        Clock::time_point _nextRetry{};
        Clock::time_point _nextClock{};
        Clock::time_point _weatherAppliedAt{};
        std::optional<proto::WorldClock> _clock;
        std::uint64_t _sent = 0;
        std::uint64_t _applied = 0;
        std::uint64_t _dropped = 0;
        std::uint64_t _clockSteps = 0;
        std::uint64_t _ambientStages = 0;
        std::uint64_t _localContainers = 0;
        // What the local player picked up from the world, by item, for a short while: a pickup the
        // server gives to someone else is taken back out again (issue #17).
        struct Gain
        {
            std::int32_t count = 0;
            Clock::time_point at{};
        };
        std::map<std::uint32_t, Gain> _worldGains;
        std::uint64_t _returned = 0;
        // '@holdworld <s>': incoming events wait that long, as on a slow link, so two players can
        // pick up the same thing before either hears of the other (dev test of issue #17).
        Clock::time_point _holdUntil{};
        std::uint32_t _lastItemBase = 0;  // what '@item' went for, so '@count' alone counts it

        // Dropped items (issue #6). A drop's reference is numbered per copy, so it is named by a shared
        // ID, and each copy keeps its own reference for it.
        struct Drop
        {
            proto::DroppedItem item;
            std::uint32_t local = 0;  // this copy's reference, 0 while not placed here
            std::shared_ptr<std::atomic<std::uint32_t>> added;    // placing: AddItem finished
            std::shared_ptr<std::atomic<std::uint32_t>> dropped;  // placing: DropObject's reference
            Clock::time_point started{};
        };
        struct DropSeen  // the player lost an item: a dropped one shows up nearby
        {
            std::uint32_t base = 0;
            std::int32_t count = 0;
            std::uint32_t ref = 0;  // the reference the drop made, from the inventory event
            Clock::time_point at{};
        };
        struct Placing  // what placing a drop does to the player's inventory, not to report
        {
            std::uint32_t base = 0;
            std::int32_t gains = 0;
            std::int32_t losses = 0;
            Clock::time_point until{};
        };
        std::map<std::uint32_t, Drop> _drops;              // by shared ID
        std::map<std::uint32_t, std::uint32_t> _dropOfRef;  // this copy's reference -> shared ID
        std::vector<DropSeen> _dropsSeen;
        std::vector<Placing> _placing;
        std::uint32_t _dropSequence = 0;
        std::uint32_t _lastPlacedDrop = 0;  // for '@takedrop'
        std::shared_ptr<std::atomic<std::uint32_t>> _dropProbe;  // '@drop': what DropObject gave
        std::uint64_t _dropsSent = 0;
        std::uint64_t _dropsPlaced = 0;
        std::vector<std::pair<std::uint32_t, std::shared_ptr<std::atomic<std::int32_t>>>> _counts;
        std::set<std::uint32_t> _awaitedContainers;  // applied here: log what the engine reports
        std::map<std::uint32_t, std::int32_t> _knownLocks;  // the event also fires when nothing changed
        // Every change this copy made or applied. Loading a save puts the world back to how the save
        // has it, so these are applied again once the player is back in the world.
        world::Ledger _seen;
        bool _wasReady = false;
        bool _everReady = false;
        std::uint64_t _replays = 0;
        Clock::time_point _nextLog{};
    };
}
