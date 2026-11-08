#include "cosync/shared/WorldSync.h"

#include <algorithm>
#include <cmath>

namespace cosync::world
{
    namespace
    {
        using proto::WorldEventKind;

        bool SameChange(const proto::WorldEvent& a, const proto::WorldEvent& b) noexcept
        {
            return a.kind == b.kind && a.formId == b.formId && a.itemId == b.itemId && a.value == b.value;
        }
    }

    bool Ledger::Apply(const proto::WorldEvent& event)
    {
        if (event.kind == WorldEventKind::ItemReturned || event.kind == WorldEventKind::ContainerRemaining) return true;
        const auto kind = static_cast<std::uint8_t>(event.kind);
        // A quest keeps every stage it reached; anything else has one current value per item.
        const bool counts = event.kind == WorldEventKind::ContainerItem || event.kind == WorldEventKind::NpcContainerItem;
        const auto detail = event.kind == WorldEventKind::QuestStage ? static_cast<std::uint32_t>(event.value) : counts ? event.itemId : 0u;
        const Key key{ kind, event.formId, detail };
        const auto it = _entries.find(key);
        if (it != _entries.end()) {
            if (counts) {
                it->second.event.value += event.value;
                if (it->second.event.value == 0) _entries.erase(it);
            } else {
                it->second.event = event;
                it->second.order = _nextOrder++;
            }
            return true;
        }
        if (_entries.size() >= kMaxEntries) return false;
        _entries.emplace(key, Entry{ _nextOrder++, event });
        return true;
    }

    std::int32_t Ledger::ContainerTotal(std::uint32_t container, std::uint32_t item, WorldEventKind kind) const
    {
        const auto it = _entries.find({ static_cast<std::uint8_t>(kind), container, item });
        return it == _entries.end() ? 0 : it->second.event.value;
    }

    // What of a history total this copy still has to apply: all of it on a fresh join, only what it
    // missed after resuming a dropped connection.
    std::int32_t MissingContainerChange(const Ledger& known, const proto::WorldEvent& history)
    {
        return history.value - known.ContainerTotal(history.formId, history.itemId, history.kind);
    }

    std::vector<proto::WorldEvent> Ledger::Snapshot() const
    {
        std::vector<const Entry*> entries;
        entries.reserve(_entries.size());
        for (const auto& [key, entry] : _entries) entries.push_back(&entry);
        std::ranges::sort(entries, {}, &Entry::order);
        std::vector<proto::WorldEvent> out;
        out.reserve(entries.size());
        for (const auto* entry : entries) out.push_back(entry->event);
        return out;
    }

    std::int32_t ContainerStock::Take(std::uint32_t container, std::uint32_t item, std::int32_t taken, std::int32_t remaining)
    {
        const auto key = (static_cast<std::uint64_t>(container) << 32) | item;
        const auto it = _known.find(key);
        std::int32_t excess = 0;
        // The sender had remaining + taken before taking. More than the container held since the last
        // report means part of it was taken twice.
        if (taken > 0 && it != _known.end() && remaining + taken > it->second) excess = std::min(taken, remaining + taken - it->second);
        if (it != _known.end() || _known.size() < kMaxEntries) _known[key] = remaining;
        return excess;
    }

    bool Pickups::Claim(std::uint64_t key, PlayerId player)
    {
        const auto it = _takenBy.find(key);
        if (it != _takenBy.end()) return it->second == player;
        if (_takenBy.size() < kMaxEntries) _takenBy.emplace(key, player);
        return true;
    }

    void EchoFilter::Expect(const proto::WorldEvent& event, std::uint64_t nowMs)
    {
        std::erase_if(_pending, [nowMs](const Pending& p) { return p.expires <= nowMs; });
        if (_pending.size() >= kMaxPending) _pending.erase(_pending.begin());
        _pending.push_back({ event, nowMs + kExpiryMs });
    }

    bool EchoFilter::Consume(const proto::WorldEvent& event, std::uint64_t nowMs)
    {
        std::erase_if(_pending, [nowMs](const Pending& p) { return p.expires <= nowMs; });
        const auto it = std::ranges::find_if(_pending, [&](const Pending& p) { return SameChange(p.event, event); });
        if (it == _pending.end()) return false;
        _pending.erase(it);
        return true;
    }

    bool NeedsClockStep(float localDays, float remoteDays) noexcept
    {
        return std::isfinite(localDays) && std::isfinite(remoteDays) && std::abs(localDays - remoteDays) > kClockToleranceDays;
    }
}
