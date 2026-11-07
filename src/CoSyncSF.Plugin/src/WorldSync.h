#pragma once

#include "cosync/net/NetworkWorker.h"
#include "cosync/shared/WorldSync.h"

#include <chrono>
#include <cstdint>
#include <deque>
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
        void OnAuthority(PlayerId authority) noexcept { _authority = authority; }
        void Reset(const char* why);

        // '@door' and '@item': the local player activates the nearest one (DevHarness).
        bool RunStep(const std::string& step);

    private:
        using Clock = std::chrono::steady_clock;

        // What the local player touched, until the engine shows what became of it.
        struct Watch
        {
            std::uint32_t ref = 0;
            std::uint32_t base = 0;
            std::uint8_t doorStateBefore = 0;  // 0 = not a door
            Clock::time_point until{};
        };
        // Read back from the engine after applying, so a log shows what really happened.
        struct Verify
        {
            proto::WorldEvent event;
            Clock::time_point at{};
        };

        void DrainActivations(Clock::time_point now);
        void UpdateWatches(net::NetworkWorker& worker, Clock::time_point now);
        void ApplyPending(Clock::time_point now);
        void RunVerifies(Clock::time_point now);
        void PublishClock(net::NetworkWorker& worker, const Context& context, Clock::time_point now);
        void ApplyClock(const Context& context, Clock::time_point now);
        // False to try again later: the thing is not somewhere it can be changed yet.
        [[nodiscard]] bool Apply(const proto::WorldEvent& event, Clock::time_point now);

        PlayerId _authority = kInvalidPlayerId;
        bool _sinkInstalled = false;
        world::EchoFilter _echo;
        std::vector<Watch> _watches;
        std::vector<proto::WorldEvent> _outgoing;
        std::deque<proto::WorldEvent> _pending;
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
        std::set<std::uint32_t> _awaitedContainers;  // applied here: log what the engine reports
        Clock::time_point _nextLog{};
    };
}
