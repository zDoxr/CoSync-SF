#pragma once

#include "cosync/shared/Protocol.h"

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

// Rules for shared world state (docs/WORLD_SYNC.md), free of the game so they can be tested.
namespace cosync::world
{
    // Everything that has happened to the world this session, in order, for players who join
    // later. One entry per thing that can change, so a door opened fifty times costs one entry.
    //   DoorOpen, RefRemoved: the newest report wins.
    //   QuestStage:           one entry per stage, stages are only ever added.
    //   ContainerItem:        counts add up, and an entry that reaches zero goes away.
    //   ItemReturned:         an answer to one player, not world state: never kept.
    class Ledger
    {
    public:
        static constexpr std::size_t kMaxEntries = 16384;

        // False when the ledger is full and the event could not be remembered.
        bool Apply(const proto::WorldEvent& event);
        [[nodiscard]] std::vector<proto::WorldEvent> Snapshot() const;
        [[nodiscard]] std::size_t Size() const noexcept { return _entries.size(); }
        void Clear() noexcept { _entries.clear(); }
        // The total change counted so far for that item in that container, 0 if none.
        [[nodiscard]] std::int32_t ContainerTotal(std::uint32_t container, std::uint32_t item) const;

    private:
        using Key = std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>;
        struct Entry
        {
            std::uint64_t order = 0;
            proto::WorldEvent event;
        };
        std::map<Key, Entry> _entries;
        std::uint64_t _nextOrder = 0;
    };

    // A history entry for a container is the session's total. After resuming a dropped connection
    // this copy has applied part of it already, and applying the whole would hand out items twice.
    [[nodiscard]] std::int32_t MissingContainerChange(const Ledger& known, const proto::WorldEvent& history);

    // An event applied from the network makes the engine report the same change. Those reports must
    // not be sent back, or two copies would keep telling each other the same thing.
    class EchoFilter
    {
    public:
        static constexpr std::uint64_t kExpiryMs = 8000;
        static constexpr std::size_t kMaxPending = 1024;

        void Expect(const proto::WorldEvent& event, std::uint64_t nowMs);
        // True when this report is the echo of something applied, and uses that expectation up.
        [[nodiscard]] bool Consume(const proto::WorldEvent& event, std::uint64_t nowMs);

    private:
        struct Pending
        {
            proto::WorldEvent event;
            std::uint64_t expires = 0;
        };
        std::vector<Pending> _pending;
    };

    // A container event names only the container and the item, and runtime FF references are numbered
    // per copy, so the same number can be a different object elsewhere. Those stay local until the
    // event can prove what it refers to.
    // Things taken from the world, by who took them first (issue #17). Two players picking up the same
    // item at once would each get it. The first report wins, and a later one from someone else is
    // answered with what to give back.
    class Pickups
    {
    public:
        static constexpr std::size_t kMaxEntries = 16384;

        // True when this player may keep what they took: they were first, or it was not a pickup.
        // `key` tells the kind of reference apart: a data-file reference or a shared drop ID.
        [[nodiscard]] bool Claim(std::uint64_t key, PlayerId player);
        [[nodiscard]] std::size_t Size() const noexcept { return _takenBy.size(); }

    private:
        std::map<std::uint64_t, PlayerId> _takenBy;
    };

    [[nodiscard]] constexpr bool CanShareContainer(std::uint32_t formId) noexcept
    {
        return formId != 0 && !proto::IsRuntimeFormId(formId);
    }

    // Clocks run at one rate on every copy, so they are only stepped when they have really parted.
    inline constexpr float kClockToleranceDays = 2.0f / (24.0f * 60.0f);  // two game minutes
    [[nodiscard]] bool NeedsClockStep(float localDays, float remoteDays) noexcept;
}
