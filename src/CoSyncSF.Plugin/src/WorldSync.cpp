#include "WorldSync.h"

#include "Avatars.h"
#include "DevHarness.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <mutex>

namespace cosync::plugin::worldsync
{
    namespace
    {
        using namespace std::chrono_literals;
        using proto::WorldEvent;
        using proto::WorldEventKind;

        constexpr std::uint64_t kGetOpenStateId = 47134;  // 0x5CDC70: 1 open, 2 opening, 3 closed, 4 closing
        constexpr std::uint64_t kIsStageDoneId = 68530;   // 0xD0DBD0; CommonLibSF has ID 0 for this game version
        constexpr std::uint64_t kSkyId = 938081;              // 0x61DF6F0, read by Weather.GetCurrentWeather
        constexpr std::uint64_t kGetLockId = 64290;           // 0xBA0610, ObjectReference.IsLocked: +12 bit 0 locked, +13 level
        constexpr std::uint64_t kDestructionStageId = 47007;  // 0x5C39A0: crashes on a base without the component
        constexpr std::uint64_t kDestructibleKeyId = 46986;   // 0x5BF6D0: the name BGSDestructibleObject_Component
        constexpr std::uint64_t kFindComponentId = 47392;     // 0x5DDB20: (form, key), null when it has none
        // Both identified at runtime by what fired after a scripted action, payloads included (RE 12.11).
        constexpr std::uint64_t kContainerChangedId = 64118;  // u32 from, to, item, count
        constexpr std::uint64_t kQuestStageId = 64161;        // u32 quest, stage
        constexpr std::uint64_t kLockChangedId = 64148;       // the reference, then other words
        constexpr std::uint32_t kPlayerRef = 0x14;
        // quest+0x114, as Quest.IsRunning (0x20CDCA0) and Quest.IsActive (0x20CDC20) read them.
        constexpr std::size_t kQuestFlags = 0x114;
        constexpr std::size_t kQuestStopping = 0x338;
        constexpr std::size_t kQuestStarting = 0x32C;
        constexpr auto kWatchFor = 4s;
        constexpr auto kVerifyAfter = 3s;
        constexpr auto kRetryEvery = 2s;
        constexpr auto kClockEvery = 5s;
        constexpr auto kWeatherHold = 30s;
        constexpr auto kLogEvery = 30s;
        constexpr std::size_t kMaxPending = 16384;
        constexpr std::size_t kMaxPerFrame = 16;

        struct RawEvent
        {
            std::uint32_t a, b, c, d;
        };
        // TESContainerChangedEvent (ID 64118): from, to, item, count, then the reference that was moved
        // when there is one, such as the one a drop made.
        struct ContainerEvent
        {
            std::uint32_t a, b, c, d, ref;
        };

        std::mutex g_activationLock;
        std::vector<std::uint32_t> g_activations;
        std::vector<ContainerEvent> g_containerChanges;
        std::vector<RawEvent> g_questStages;
        std::vector<std::uint32_t> g_lockChanges;

        // On whatever thread activated the reference, so it only notes the form ID.
        class ActivationSink final : public RE::BSTEventSink<RE::Activation::Event>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::Activation::Event& event, RE::BSTEventSource<RE::Activation::Event>*) override
            {
                const auto* player = RE::PlayerCharacter::GetSingleton();
                if (player && event.target && static_cast<const RE::TESForm*>(event.source.get()) == player) {
                    std::scoped_lock lock(g_activationLock);
                    if (g_activations.size() < 256) g_activations.push_back(event.target->GetFormID());
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        ActivationSink g_sink;

        class ContainerSink final : public RE::BSTEventSink<ContainerEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const ContainerEvent& event, RE::BSTEventSource<ContainerEvent>*) override
            {
                std::scoped_lock lock(g_activationLock);
                if (g_containerChanges.size() < 1024) g_containerChanges.push_back(event);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        ContainerSink g_containerSink;

        class QuestSink final : public RE::BSTEventSink<RawEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RawEvent& event, RE::BSTEventSource<RawEvent>*) override
            {
                std::scoped_lock lock(g_activationLock);
                if (g_questStages.size() < 256) g_questStages.push_back(event);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        QuestSink g_questSink;

        // The payload starts with a pointer to the reference, good only during the call.
        struct RefEvent
        {
            RE::TESObjectREFR* ref;
        };
        class LockSink final : public RE::BSTEventSink<RefEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RefEvent& event, RE::BSTEventSource<RefEvent>*) override
            {
                std::scoped_lock lock(g_activationLock);
                if (event.ref && g_lockChanges.size() < 256) g_lockChanges.push_back(event.ref->GetFormID());
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        LockSink g_lockSink;

        const auto g_homeShip = std::make_shared<std::atomic<std::uint32_t>>(0);
        const auto g_standIn = std::make_shared<std::atomic<std::uint32_t>>(0);
        std::uint32_t g_lastTarget = 0;

        bool InSpace(RE::TESObjectREFR* ref)
        {
            using Fn = bool (*)(RE::TESObjectREFR*, bool);
            static REL::Relocation<Fn> fn{ REL::ID(63482) };  // 0xB58D50, ObjectReference.IsInSpace
            return fn(ref, false);
        }

        RE::BSTEventSource<RawEvent>* RawSource(std::uint64_t id)
        {
            using Getter_t = RE::BSTEventSource<RawEvent>* (*)();
            return reinterpret_cast<Getter_t>(REL::ID(id).address())();
        }

        std::uint32_t QuestFlags(const RE::TESQuest* quest)
        {
            return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(quest) + kQuestFlags);
        }

        bool StageDone(const RE::TESQuest* quest, std::int32_t stage)
        {
            using Fn = bool (*)(const RE::TESQuest*, std::uint16_t);
            static REL::Relocation<Fn> fn{ REL::ID(kIsStageDoneId) };
            return fn(quest, static_cast<std::uint16_t>(stage));
        }

        bool QuestIsActive(const RE::TESQuest* quest) { return (QuestFlags(quest) & 0x800) != 0; }

        // quest+0x148 count, +0x150 objectives (0xD09230 walks them), each with its state at +40.
        // 1, 3 and 5 are the states Quest.IsObjectiveDisplayed (0x20CB700) counts as shown.
        bool QuestShowsObjective(const RE::TESQuest* quest)
        {
            const auto* bytes = reinterpret_cast<const std::byte*>(quest);
            const auto count = *reinterpret_cast<const std::uint32_t*>(bytes + 0x148);
            const auto* objectives = *reinterpret_cast<const std::byte* const* const*>(bytes + 0x150);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto state = *reinterpret_cast<const std::uint32_t*>(objectives[i] + 40);
                if (state == 1 || state == 3 || state == 5) return true;
            }
            return false;
        }

        // quest+0x138 count, +0x140 stages (0xD0DBD0), each with its index at +16 and done bit at +18.
        std::vector<std::pair<std::uint16_t, bool>> QuestStages(const RE::TESQuest* quest)
        {
            const auto* bytes = reinterpret_cast<const std::byte*>(quest);
            const auto count = *reinterpret_cast<const std::uint32_t*>(bytes + 0x138);
            const auto* stages = *reinterpret_cast<const std::byte* const* const*>(bytes + 0x140);
            std::vector<std::pair<std::uint16_t, bool>> out;
            for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
                out.emplace_back(*reinterpret_cast<const std::uint16_t*>(stages[i] + 16), (*reinterpret_cast<const std::uint8_t*>(stages[i] + 18) & 1) != 0);
            }
            return out;
        }

        bool QuestIsRunning(const RE::TESQuest* quest)
        {
            const auto* bytes = reinterpret_cast<const std::byte*>(quest);
            const auto flags = QuestFlags(quest);
            return (flags & 1) != 0 && (flags & 0x80) == 0 && *reinterpret_cast<void* const*>(bytes + kQuestStopping) == nullptr &&
                   *reinterpret_cast<const std::uint8_t*>(bytes + kQuestStarting) == 0;
        }

        bool ReadWord(std::uintptr_t at, std::uint64_t& out) noexcept
        {
            __try {
                out = *reinterpret_cast<const std::uint64_t*>(at);
                return true;
            } __except (1) {
                return false;
            }
        }

        // A pointer to a reference: right vtable range, and the form type is REFR or ACHR.
        bool IsRefPointer(std::uint64_t word) noexcept
        {
            static const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (word < 0x10000 || word > 0x7FFFFFFFFFFF || (word & 7) != 0) return false;
            std::uint64_t vtable = 0, type = 0;
            if (!ReadWord(word, vtable) || !ReadWord(word + 0x28, type)) return false;
            const auto formType = static_cast<std::uint8_t>(type >> 48);
            return vtable >= image && vtable < image + 0x8000000 && (formType == 0x4A || formType == 0x4B);
        }

        // The class name the compiler stored for this object: vtable[-1] -> locator -> type descriptor.
        std::string RttiName(const void* object)
        {
            static const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::uint64_t vtable = 0, locator = 0, word = 0;
            if (!ReadWord(reinterpret_cast<std::uintptr_t>(object), vtable) || !ReadWord(vtable - 8, locator) || !ReadWord(locator + 12, word)) return "?";
            const auto* name = reinterpret_cast<const char*>(image + static_cast<std::uint32_t>(word) + 0x10);
            return std::string(name, strnlen(name, 80));
        }

        std::size_t CountRefs(std::uint64_t data)
        {
            std::size_t n = 0;
            for (std::uint64_t word = 0; n < 65536 && ReadWord(data + n * 8, word) && IsRefPointer(word); ++n) {}
            return n;
        }

        // Found by what it contains, with every entry checked, because a test step must not take the
        // game down: walking the array the plain way crashed on 2026-11-07, cause not yet known.
        std::vector<RE::TESObjectREFR*> CellRefs(const RE::TESObjectCELL* cell)
        {
            static std::size_t offset = 0;
            const auto base = reinterpret_cast<std::uintptr_t>(cell);
            if (offset == 0) {
                std::size_t best = 0;
                for (std::size_t at = 0x40; at < 0x300; at += 8) {
                    std::uint64_t data = 0;
                    if (!ReadWord(base + at, data)) continue;
                    if (const auto n = CountRefs(data); n > best) {
                        best = n;
                        offset = at;
                    }
                }
                log::Info("[world] cell references found at +{:#x}: {} in the first run", offset, best);
                if (best < 2) offset = 0;
            }
            std::vector<RE::TESObjectREFR*> refs;
            std::uint64_t data = 0;
            if (offset == 0 || !ReadWord(base + offset, data)) return refs;
            const auto n = CountRefs(data);
            refs.reserve(n);
            for (std::size_t i = 0; i < n; ++i) refs.push_back(*reinterpret_cast<RE::TESObjectREFR* const*>(data + i * 8));
            return refs;
        }

        std::uint64_t NowMs()
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        std::uint8_t DoorState(RE::TESObjectREFR* ref)
        {
            using Fn = std::uint8_t (*)(RE::TESObjectREFR*);
            static REL::Relocation<Fn> fn{ REL::ID(kGetOpenStateId) };
            return fn(ref);
        }

        std::uint32_t BaseId(const RE::TESObjectREFR* ref)
        {
            const auto base = ref->GetBaseObject();
            return base ? base->GetFormID() : 0;
        }

        bool IsDoor(const RE::TESObjectREFR* ref)
        {
            const auto base = ref->GetBaseObject();
            return base && base->Is(RE::FormType::kDOOR);
        }

        bool IsLooseItem(const RE::TESObjectREFR* ref)
        {
            const auto base = ref->GetBaseObject();
            if (!base) return false;
            switch (static_cast<RE::FormType>(base->GetFormType())) {
            case RE::FormType::kARMO:
            case RE::FormType::kWEAP:
            case RE::FormType::kMISC:
            case RE::FormType::kALCH:
            case RE::FormType::kBOOK:
            case RE::FormType::kAMMO:
            case RE::FormType::kKEYM:
                return true;
            default:
                return false;
            }
        }

        // Runtime FF references are numbered per copy of a save, so the number alone is no proof.
        RE::TESObjectREFR* LookupRef(std::uint32_t formId, std::uint32_t base)
        {
            auto* form = RE::TESForm::LookupByID(formId);
            if (!form) return nullptr;
            auto* ref = form->As<RE::TESObjectREFR>();
            if (!ref) return nullptr;
            if (proto::IsRuntimeFormId(formId) && base != 0 && BaseId(ref) != base) return nullptr;
            return ref;
        }

        bool IsGone(const RE::TESObjectREFR* ref) { return ref->IsDisabled() || ref->IsDeleted() || !ref->parentCell; }

        RE::Calendar* Calendar()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar && calendar->gameDaysPassed && calendar->gameHour ? calendar : nullptr;
        }

        // Weather forms have no script object to call SetActive on, so this is the engine's own command.
        void SetWeather(std::uint32_t id) { dev::RunConsole(std::format("fw {:08X}", id)); }

        std::uint32_t CurrentWeather()
        {
            auto* sky = *REL::Relocation<RE::Sky**>{ REL::ID(kSkyId) };
            return sky && sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
        }

        template <class Filter>
        RE::TESObjectREFR* Nearest(Filter filter, const RE::TESObjectCELL* cell = nullptr)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) return nullptr;
            if (!cell) cell = player->parentCell;
            const auto origin = player->GetPosition();
            RE::TESObjectREFR* best = nullptr;
            float bestDistance = std::numeric_limits<float>::max();
            std::size_t matches = 0, gone = 0;
            for (auto* ref : CellRefs(cell)) {
                if (!filter(ref)) continue;
                ++matches;
                if (IsGone(ref)) {
                    ++gone;
                    continue;
                }
                const auto at = ref->GetPosition();
                const auto d = std::hypot(at.x - origin.x, at.y - origin.y, at.z - origin.z);
                if (!best || d < bestDistance) {
                    bestDistance = d;
                    best = ref;
                }
            }
            log::Info("[world] search: {} match, {} of them gone, nearest {:.1f}", matches, gone, bestDistance);
            return best;
        }

        // 1 locked, 0 unlocked, -1 no lock.
        int LockState(RE::TESObjectREFR* ref)
        {
            using Fn = const std::byte* (*)(RE::TESObjectREFR*);
            static REL::Relocation<Fn> fn{ REL::ID(kGetLockId) };
            const auto* lock = fn(ref);
            return lock ? static_cast<int>(*reinterpret_cast<const std::uint8_t*>(lock + 12) & 1) : -1;
        }

        // -1 for something that cannot be destroyed.
        int DestructionStage(RE::TESObjectREFR* ref)
        {
            using Key_t = const void* (*)();
            using Find_t = const void* (*)(const void*, const void*);
            using Stage_t = int (*)(RE::TESObjectREFR*);
            static REL::Relocation<Key_t> key{ REL::ID(kDestructibleKeyId) };
            static REL::Relocation<Find_t> find{ REL::ID(kFindComponentId) };
            static REL::Relocation<Stage_t> stage{ REL::ID(kDestructionStageId) };
            const auto base = ref->GetBaseObject();
            if (!base || !find(base.get(), key())) return -1;
            return stage(ref);
        }

        // ObjectReference.IsDestroyed (0x208BAA0) reads this flag.
        bool IsDestroyed(const RE::TESObjectREFR* ref)
        {
            return (*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(ref) + 0x20) & 0x800000) != 0;
        }

        bool IsContainer(const RE::TESObjectREFR* ref)
        {
            const auto base = ref->GetBaseObject();
            return base && base->Is(RE::FormType::kCONT);
        }
    }

    void Replication::Reset(const char* why)
    {
        log::Info("[world] reset: {}", why);
        _watches.clear();
        _outgoing.clear();
        _pending.clear();
        _verifies.clear();
        _clock.reset();
        _seen.Clear();
        _knownLocks.clear();
        _drops.clear();
        _dropOfRef.clear();
        _dropsSeen.clear();
        _placing.clear();
        _everReady = false;
        _wasReady = false;
    }

    void Replication::ReplayAfterLoad()
    {
        // Container changes are counts added to whatever was there, and a save made after the change
        // already holds it, so applying them again could hand out items twice. Those stay as loaded.
        std::size_t queued = 0;
        _knownLocks.clear();
        // Drops: a reference the load took away is placed again.
        for (auto& [sharedId, d] : _drops) {
            auto* form = d.local != 0 ? RE::TESForm::LookupByID(d.local) : nullptr;
            auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (d.local != 0 && (!ref || IsGone(ref) || BaseId(ref) != d.item.baseId)) {
                _dropOfRef.erase(d.local);
                d.local = 0;
                d.started = {};
            }
        }
        for (const auto& e : _seen.Snapshot()) {
            if (e.kind == WorldEventKind::ContainerItem) continue;
            _pending.push_back({ e, true });  // it was applied here before, so it may start a quest again
            ++queued;
        }
        ++_replays;
        log::Info("[world] back in the world after a load: applying {} known changes again", queued);
    }

    void Replication::OnEvents(const proto::WorldEvents& events)
    {
        for (const auto& e : events.events) {
            if (_pending.size() >= kMaxPending) {
                ++_dropped;
                continue;
            }
            // History from the server has no sender and is treated as the authority's.
            const bool history = events.playerId == kInvalidPlayerId;
            const bool mayStart = events.playerId == _authority || history;
            if (history && e.kind == WorldEventKind::ContainerItem) {
                const auto missing = world::MissingContainerChange(_seen, e);
                if (missing != e.value) {
                    log::Info("[world] history: container {:08X} item {:08X} total {:+}, {:+} already applied here, applying {:+}", e.formId, e.itemId, e.value,
                              e.value - missing, missing);
                }
                if (missing == 0) continue;
                auto part = e;
                part.value = missing;
                _pending.push_back({ part, mayStart });
                continue;
            }
            _pending.push_back({ e, mayStart });
        }
    }

    void Replication::OnClock(const proto::WorldClock& clock) { _clock = clock; }

    void Replication::OnDrop(const proto::DroppedItem& drop)
    {
        if (_drops.size() >= 4096 && !_drops.contains(drop.sharedId)) return;
        auto& d = _drops[drop.sharedId];
        d.item = drop;
    }

    void Replication::ForgetDrop(std::uint32_t sharedId)
    {
        const auto it = _drops.find(sharedId);
        if (it == _drops.end()) return;
        if (it->second.local != 0) _dropOfRef.erase(it->second.local);
        _drops.erase(it);
    }

    // The player lost an item and a reference of it appeared next to them: a drop. Anything else
    // (eaten, sold, given) leaves no reference, and is forgotten after 2 s.
    void Replication::DetectDrops(net::NetworkWorker& worker, const Context& context, Clock::time_point now)
    {
        if (_dropsSeen.empty()) return;
        if (context.localPlayer == kInvalidPlayerId) {
            _dropsSeen.clear();
            return;
        }
        // The inventory event names the reference a drop made (+16, seen 2026-11-07 with DropObject's
        // FF0285E2). Its 3D can come a frame later, so a reference not yet there is tried again briefly.
        std::erase_if(_dropsSeen, [&](const DropSeen& seen) {
            auto* form = RE::TESForm::LookupByID(seen.ref);
            auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!ref || BaseId(ref) != seen.base || IsGone(ref)) {
                if (now - seen.at < 2s) return false;
                log::Info("[world] drop of {:08X} as {:08X}: no such reference in the world", seen.base, seen.ref);
                return true;
            }
            if (_dropOfRef.contains(seen.ref)) return true;
            const auto p = ref->GetPosition();
            const proto::DroppedItem drop{ kInvalidPlayerId, proto::MakeSharedId(context.localPlayer, ++_dropSequence), seen.base,
                                           std::clamp(seen.count, 1, 0xFFFF), context.locationId, { p.x, p.y, p.z } };
            _drops[drop.sharedId] = { drop, seen.ref, {}, {}, now };
            _dropOfRef[seen.ref] = drop.sharedId;
            worker.SubmitDroppedItem(drop);
            ++_dropsSent;
            log::Info("[world] dropped {:08X} x{} as {:08X}, shared {:08X} at {:.1f} {:.1f} {:.1f}", seen.base, seen.count, seen.ref, drop.sharedId, p.x, p.y, p.z);
            return true;
        });
    }

    // Another player's drop, placed where they left it: the item is added to the player and dropped,
    // which makes the reference, then moved there. Only in the same location.
    void Replication::PlaceDrops(const Context& context, Clock::time_point now)
    {
        std::erase_if(_placing, [now](const Placing& p) { return now >= p.until; });
        std::size_t inFlight = 0;
        for (auto& [sharedId, d] : _drops) {
            if (d.local != 0) continue;
            if (d.dropped) {
                const auto id = d.dropped->load();
                if (id == 0) {
                    if (now - d.started < 10s) {
                        ++inFlight;
                        continue;
                    }
                    log::Info("[world] drop {:08X}: no reference after 10 s", sharedId);
                } else if (id != 1) {
                    d.local = id;
                    _dropOfRef[id] = sharedId;
                    _lastPlacedDrop = id;
                    avatars::PlaceRef(id, d.item.position, {});
                    if (++_dropsPlaced <= 50) {
                        log::Info("[world] placed drop {:08X} ({:08X} x{} from player {}) as {:08X} at {:.1f} {:.1f} {:.1f}", sharedId, d.item.baseId, d.item.count,
                                  d.item.playerId, id, d.item.position.x, d.item.position.y, d.item.position.z);
                    }
                } else {
                    log::Info("[world] drop {:08X}: DropObject failed", sharedId);
                }
                d.added.reset();
                d.dropped.reset();
                continue;
            }
            if (d.added) {
                if (d.added->load() == 0 && now - d.started < 10s) {
                    ++inFlight;
                    continue;
                }
                d.dropped = std::make_shared<std::atomic<std::uint32_t>>(0);
                if (!avatars::RequestDropFromPlayer(d.item.baseId, d.item.count, d.dropped)) d.dropped->store(1);
                ++inFlight;
                continue;
            }
            if (inFlight >= 2 || d.item.locationId != context.locationId || now - d.started < 5s) continue;
            d.started = now;
            d.added = std::make_shared<std::atomic<std::uint32_t>>(0);
            _placing.push_back({ d.item.baseId, d.item.count, d.item.count, now + 15s });
            if (!avatars::AddToPlayer(d.item.baseId, d.item.count, d.added)) {
                d.added.reset();
                continue;
            }
            ++inFlight;
        }
    }

    void Replication::DrainActivations(Clock::time_point now)
    {
        std::vector<std::uint32_t> ids, locks;
        std::vector<ContainerEvent> changes;
        std::vector<RawEvent> stages;
        {
            std::scoped_lock lock(g_activationLock);
            ids.swap(g_activations);
            changes.swap(g_containerChanges);
            stages.swap(g_questStages);
            locks.swap(g_lockChanges);
        }
        for (const auto id : locks) {
            // Runtime references are numbered per copy (issue #6). A change a script makes on every
            // copy is sent too, and costs nothing: the receiver already has that state.
            if (proto::IsRuntimeFormId(id)) continue;
            auto* ref = LookupRef(id, 0);
            if (!ref) continue;
            const auto state = LockState(ref);
            if (state < 0) continue;
            const auto known = _knownLocks.find(id);
            const bool changed = known == _knownLocks.end() || known->second != state;
            _knownLocks[id] = state;
            const WorldEvent e{ WorldEventKind::LockState, id, BaseId(ref), state };
            if (changed && !_echo.Consume(e, NowMs())) _outgoing.push_back(e);
        }
        for (const auto& c : changes) {
            // Only what the local player moved. What an apply does on this copy never involves the
            // player, so it cannot come back round.
            const auto other = c.a == kPlayerRef ? c.b : c.b == kPlayerRef ? c.a : 0;
            if ((c.a == 0 && c.b == kPlayerRef) || (c.a == kPlayerRef && c.b == 0)) {
                static int logged = 0;
                if (++logged <= 30) log::Info("[world] engine: player {} item {:08X} x{}", c.a == 0 ? "gained" : "lost", c.c, c.d);
                const bool gained = c.a == 0;
                const auto placing = std::ranges::find_if(_placing, [&](const Placing& p) {
                    return p.base == c.c && now < p.until && (gained ? p.gains : p.losses) > 0;
                });
                if (placing != _placing.end()) {
                    (gained ? placing->gains : placing->losses) -= static_cast<std::int32_t>(c.d);
                    continue;
                }
                if (!gained && c.c != 0 && proto::IsRuntimeFormId(c.ref)) _dropsSeen.push_back({ c.c, static_cast<std::int32_t>(c.d), c.ref, now });
            }
            if (c.a == 0 && c.b == kPlayerRef && c.c != 0) {
                auto& gain = _worldGains[c.c];
                gain.count = (now - gain.at < 2s ? gain.count : 0) + static_cast<std::int32_t>(c.d);
                gain.at = now;
            }
            if (c.a == 0 || c.b == 0) {
                if (_awaitedContainers.contains(c.a != 0 ? c.a : c.b)) {
                    log::Info("[world] engine: container {:08X} {} item {:08X} x{}", c.a != 0 ? c.a : c.b, c.a != 0 ? "lost" : "gained", c.c, c.d);
                }
            }
            if (other == 0) continue;
            if (!world::CanShareContainer(other)) {
                ++_localContainers;
                continue;
            }
            const auto* ref = LookupRef(other, 0);
            if (!ref || !IsContainer(ref)) continue;
            const auto count = static_cast<std::int32_t>(c.d);
            _outgoing.push_back({ WorldEventKind::ContainerItem, other, c.c, c.b == kPlayerRef ? -count : count });
        }
        for (const auto& s : stages) {
            const auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(s.a);
            if (!quest) continue;
            const proto::WorldEvent e{ WorldEventKind::QuestStage, s.a, 0, static_cast<std::int32_t>(s.b & 0xFFFF) };
            if (_echo.Consume(e, NowMs())) continue;
            // Ambient quests run on each copy for themselves. Only what a player can see is shared.
            if (!QuestShowsObjective(quest)) {
                ++_ambientStages;
                continue;
            }
            _outgoing.push_back(e);
        }
        for (const auto id : ids) {
            auto* ref = LookupRef(id, 0);
            if (!ref) continue;
            const bool door = IsDoor(ref);
            if (!door && !IsLooseItem(ref)) continue;
            _watches.push_back({ id, BaseId(ref), door ? DoorState(ref) : std::uint8_t{ 0 }, now + kWatchFor });
        }
    }

    void Replication::UpdateWatches(net::NetworkWorker&, Clock::time_point now)
    {
        std::erase_if(_watches, [&](const Watch& w) {
            auto* form = RE::TESForm::LookupByID(w.ref);
            auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!ref || w.doorStateBefore == 0) {
                if (!ref || IsGone(ref)) {
                    // The inventory event can come a frame after the item goes: wait briefly for it.
                    const auto gain = _worldGains.find(w.base);
                    const bool gained = gain != _worldGains.end() && now - gain->second.at < 5s;
                    if (!gained) {
                        auto& self = const_cast<Watch&>(w);
                        if (self.goneAt == Clock::time_point{}) self.goneAt = now;
                        if (now - self.goneAt < 500ms) return false;
                    }
                    const auto count = gained ? std::clamp(gain->second.count, 0, 0xFFFF) : 0;
                    if (gained) _worldGains.erase(gain);
                    if (const auto shared = _dropOfRef.find(w.ref); shared != _dropOfRef.end()) {
                        const auto sharedId = shared->second;
                        _outgoing.push_back({ WorldEventKind::SharedRemoved, sharedId, w.base, count });
                        log::Info("[world] picked up drop {:08X} (here {:08X})", sharedId, w.ref);
                        ForgetDrop(sharedId);
                        return true;
                    }
                    _outgoing.push_back({ WorldEventKind::RefRemoved, w.ref, w.base, count });
                    return true;
                }
            } else if (const auto state = DoorState(ref); state != w.doorStateBefore && state != 0) {
                _outgoing.push_back({ WorldEventKind::DoorOpen, w.ref, w.base, state == 1 || state == 2 ? 1 : 0 });
                return true;
            }
            return now >= w.until;  // locked, refused, or still there: nothing changed
        });
    }

    bool Replication::Apply(const WorldEvent& e, bool mayStart, Clock::time_point now)
    {
        switch (e.kind) {
        case WorldEventKind::DoorOpen: {
            auto* ref = LookupRef(e.formId, e.itemId);
            if (!ref) {
                ++_dropped;
                log::Warn("[world] door {:08X} is not on this copy", e.formId);
                return true;
            }
            // Needs its cell in memory: a door further off waits until a player brings it in.
            if (!ref->parentCell || !ref->parentCell->IsAttached()) return false;
            avatars::SetOpen(e.formId, e.value != 0);
            break;
        }
        case WorldEventKind::RefRemoved: {
            auto* ref = LookupRef(e.formId, e.itemId);
            if (!ref || IsGone(ref)) return true;
            avatars::DisableNoWait(e.formId);
            break;
        }
        case WorldEventKind::LockState: {
            auto* ref = LookupRef(e.formId, e.itemId);
            if (!ref) {
                ++_dropped;
                return true;
            }
            _knownLocks[e.formId] = e.value;
            if (LockState(ref) == e.value) return true;
            avatars::SetLocked(e.formId, e.value != 0);
            break;
        }
        case WorldEventKind::Destroyed: {
            auto* ref = LookupRef(e.formId, e.itemId);
            if (!ref) {
                ++_dropped;
                return true;
            }
            if (IsDestroyed(ref) == (e.value != 0)) return true;
            avatars::SetDestroyed(e.formId, e.value != 0);
            break;
        }
        case WorldEventKind::QuestStage: {
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(e.formId);
            if (!quest) {
                ++_dropped;
                return true;
            }
            if (StageDone(quest, e.value)) return true;
            // Setting a stage starts a quest that is not running. Only quests with an objective on show
            // are reported, but each copy also picks some at random (00193266, 00193269, 0019326B on one
            // test), so one source decides, or every pick would spread to everyone.
            if (!QuestIsRunning(quest)) {
                if (!mayStart) {
                    ++_dropped;
                    log::Info("[world] quest {:08X} stage {} kept out: not running here, not from the authority", e.formId, e.value);
                    return true;
                }
                log::Info("[world] quest {:08X} starts here with stage {}", e.formId, e.value);
            }
            _echo.Expect(e, NowMs());
            avatars::SetQuestStage(e.formId, e.value);
            break;
        }
        case WorldEventKind::SharedRemoved: {
            const auto it = _drops.find(e.formId);
            if (it == _drops.end()) return true;
            if (it->second.local != 0) {
                avatars::DisableNoWait(it->second.local);
                _verifies.push_back({ { WorldEventKind::RefRemoved, it->second.local, it->second.item.baseId, 0 }, now + kVerifyAfter });
            }
            log::Info("[world] drop {:08X} was picked up by another player: removed {:08X} here", e.formId, it->second.local);
            ForgetDrop(e.formId);
            ++_applied;
            return true;
        }
        case WorldEventKind::ItemReturned: {
            // Someone else picked this up first: what this player got goes back.
            avatars::ContainerRemove(kPlayerRef, e.itemId, e.value);
            auto* item = RE::TESForm::LookupByID(e.itemId);
            auto* full = item ? starfield_cast<RE::TESFullName*>(item) : nullptr;
            const char* name = full ? full->GetFullName() : nullptr;
            avatars::Notify(std::format("Another player took {} first", name && *name ? name : "that"));
            log::Info("[world] {:08X} was taken by another player first: giving back item {:08X} x{}", e.formId, e.itemId, e.value);
            ++_returned;
            return true;
        }
        case WorldEventKind::ContainerItem: {
            const auto* ref = world::CanShareContainer(e.formId) ? LookupRef(e.formId, 0) : nullptr;
            if (!ref || !IsContainer(ref)) {
                ++_dropped;
                return true;
            }
            _awaitedContainers.insert(e.formId);
            if (e.value > 0) {
                avatars::ContainerAdd(e.formId, e.itemId, e.value);
            } else {
                avatars::ContainerRemove(e.formId, e.itemId, -e.value);
            }
            log::Info("[world] applied container {:08X} item {:08X} {:+}", e.formId, e.itemId, e.value);
            ++_applied;
            return true;
        }
        }
        ++_applied;
        _verifies.push_back({ e, now + kVerifyAfter });
        return true;
    }

    void Replication::ApplyPending(Clock::time_point now)
    {
        if (now < _nextRetry || now < _holdUntil) return;
        std::size_t budget = kMaxPerFrame;
        std::deque<Incoming> later;
        while (!_pending.empty() && budget > 0) {
            const auto e = _pending.front();
            _pending.pop_front();
            --budget;
            // Remembered for a reload whether it changed anything or this copy already had it, but
            // not when it was dropped or kept out.
            const auto dropped = _dropped;
            if (!Apply(e.event, e.mayStart, now)) {
                later.push_back(e);
            } else if (_dropped == dropped) {
                _seen.Apply(e.event);
            }
        }
        // Everything was tried and nothing moved: wait, rather than spinning on it every frame.
        if (_pending.empty() && !later.empty()) _nextRetry = now + kRetryEvery;
        _pending.insert(_pending.end(), later.begin(), later.end());
    }

    void Replication::RunVerifies(Clock::time_point now)
    {
        std::erase_if(_verifies, [&](const Verify& v) {
            if (now < v.at) return false;
            auto* ref = LookupRef(v.event.formId, v.event.itemId);
            switch (v.event.kind) {
            case WorldEventKind::DoorOpen:
                log::Info("[world] verify door {:08X}: wanted {}, engine state {}", v.event.formId, v.event.value != 0 ? "open" : "closed",
                          ref ? DoorState(ref) : 0);
                break;
            case WorldEventKind::LockState:
                log::Info("[world] verify lock {:08X}: wanted {}, engine {}", v.event.formId, v.event.value, ref ? LockState(ref) : -2);
                break;
            case WorldEventKind::Destroyed:
                log::Info("[world] verify destroyed {:08X}: wanted {}, engine {}", v.event.formId, v.event.value, ref && IsDestroyed(ref));
                break;
            case WorldEventKind::RefRemoved:
                log::Info("[world] verify removed {:08X}: {}", v.event.formId, !ref || IsGone(ref) ? "gone" : "STILL THERE");
                break;
            case WorldEventKind::QuestStage: {
                const auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(v.event.formId);
                log::Info("[world] verify quest {:08X} stage {}: {}", v.event.formId, v.event.value,
                          quest && StageDone(quest, v.event.value) ? "done" : "NOT DONE");
                break;
            }
            case WorldEventKind::ContainerItem:
            case WorldEventKind::ItemReturned:
            case WorldEventKind::SharedRemoved:
                break;
            }
            return true;
        });
    }

    void Replication::PublishClock(net::NetworkWorker& worker, const Context& context, Clock::time_point now)
    {
        if (context.localPlayer != _authority || now < _nextClock) return;
        _nextClock = now + kClockEvery;
        const auto* calendar = Calendar();
        if (!calendar) return;
        worker.SubmitWorldClock({ kInvalidPlayerId, calendar->gameDaysPassed->value, calendar->gameHour->value, context.locationId, CurrentWeather() });
    }

    void Replication::ApplyClock(const Context& context, Clock::time_point now)
    {
        if (!_clock) return;
        const auto clock = *_clock;
        _clock.reset();
        auto* calendar = Calendar();
        if (!calendar) return;
        const auto localHour = calendar->gameHour->value;
        if (world::NeedsClockStep(calendar->gameDaysPassed->value, clock.gameDaysPassed) ||
            world::NeedsClockStep(localHour / 24.0f, clock.gameHour / 24.0f)) {
            log::Info("[world] clock step: days {:.4f} -> {:.4f}, hour {:.3f} -> {:.3f}", calendar->gameDaysPassed->value, clock.gameDaysPassed, localHour,
                      clock.gameHour);
            calendar->gameDaysPassed->value = clock.gameDaysPassed;
            calendar->gameHour->value = clock.gameHour;
            ++_clockSteps;
        }
        // A weather belongs to a place, and changing it takes a moment to show, so give it time.
        if (clock.weather != 0 && clock.locationId == context.locationId && clock.weather != CurrentWeather() &&
            now - _weatherAppliedAt >= kWeatherHold) {
            _weatherAppliedAt = now;
            log::Info("[world] weather {:08X} -> {:08X}", CurrentWeather(), clock.weather);
            SetWeather(clock.weather);
        }
    }

    void Replication::Tick(net::NetworkWorker& worker, const Context& context)
    {
        _worker = &worker;
        if (!_sinkInstalled) {
            _sinkInstalled = true;
            RE::Activation::Event::GetEventSource()->RegisterSink(&g_sink);
            reinterpret_cast<RE::BSTEventSource<ContainerEvent>*>(RawSource(kContainerChangedId))->RegisterSink(&g_containerSink);
            RawSource(kQuestStageId)->RegisterSink(&g_questSink);
            reinterpret_cast<RE::BSTEventSource<RefEvent>*>(RawSource(kLockChangedId))->RegisterSink(&g_lockSink);
            log::Info("[world] watching what the local player activates");
        }
        if (!context.ready) {
            _wasReady = false;
            return;
        }
        if (!_wasReady) {
            _wasReady = true;
            if (_everReady) ReplayAfterLoad();
            _everReady = true;
        }
        const auto now = Clock::now();
        DrainActivations(now);
        DetectDrops(worker, context, now);
        PlaceDrops(context, now);
        UpdateWatches(worker, now);
        ApplyPending(now);
        RunVerifies(now);
        PublishClock(worker, context, now);
        ApplyClock(context, now);
        if (_dropProbe && _dropProbe->load() != 0) {
            const auto id = _dropProbe->load();
            _dropProbe.reset();
            auto* form = id > 1 ? RE::TESForm::LookupByID(id) : nullptr;
            auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (ref) {
                const auto p = ref->GetPosition();
                const auto q = player->GetPosition();
                bool inCell = false;
                if (ref->parentCell) {
                    for (auto* r : CellRefs(ref->parentCell)) inCell |= r == ref;
                }
                log::Info("[world] DropObject gave {:08X} base {:08X} cell {:08X} (player's {:08X}), listed in its cell {}, {:.0f} units from the player", id, BaseId(ref),
                          ref->parentCell ? ref->parentCell->GetFormID() : 0, player->parentCell ? player->parentCell->GetFormID() : 0, inCell,
                          std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z)));
            } else {
                log::Info("[world] DropObject gave {:08X}, not a reference", id);
            }
        }
        std::erase_if(_counts, [](const auto& c) {
            const auto n = c.second->load();
            if (n == -2) return false;
            const auto* form = RE::TESForm::LookupByID(c.first);
            // The engine's own inventory, read directly, beside what the script call said.
            std::uint64_t direct = 0, entries = 0;
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                player->ForEachInventoryItem([&](const RE::BGSInventoryItem& item) {
                    if (++entries <= 5 && item.object) log::Info("[world]   inventory entry {:08X} stacks {}", item.object->GetFormID(), item.stacks.size());
                    if (item.object && item.object->GetFormID() == c.first) {
                        for (const auto& s : item.stacks) direct += s.count;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            log::Info("[world] the player has {} of item {:08X} ({}); inventory list says {} ({} entries)", n, c.first, form ? RttiName(form) : "?", direct, entries);
            return true;
        });
        if (!_outgoing.empty()) {
            for (const auto& e : _outgoing) {
                log::Info("[world] report kind {} form {:08X} value {}", static_cast<int>(e.kind), e.formId, e.value);
            }
            _sent += _outgoing.size();
            for (const auto& e : _outgoing) _seen.Apply(e);
            worker.SubmitWorldEvents(_outgoing);
            _outgoing.clear();
        }
        if (now >= _nextLog) {
            _nextLog = now + kLogEvery;
            log::Info("[world] {} ambient quest stages and {} runtime container changes kept local", _ambientStages, _localContainers);
            log::Info("[world] {} reported, {} applied, {} waiting, {} dropped, {} clock steps; day {:.3f} hour {:.2f} weather {:08X}", _sent, _applied, _pending.size(),
                      _dropped, _clockSteps, Calendar() ? Calendar()->gameDaysPassed->value : 0.0f, Calendar() ? Calendar()->gameHour->value : 0.0f, CurrentWeather());
        }
    }

    bool Replication::RunStep(const std::string& step)
    {
        RE::TESObjectREFR* ref = nullptr;
        if (step == "@quests") {
            // TESDataHandler's form arrays read as empty here (RE 12.11), so ask the form table itself.
            std::size_t quests = 0, running = 0, active = 0, shown = 0, weathers = 0;
            std::string activeIds, weatherIds;
            for (std::uint32_t id = 1; id < 0x400000; ++id) {
                auto* form = RE::TESForm::LookupByID(id);
                if (!form) continue;
                if (form->Is(RE::FormType::kQUST)) {
                    const auto* quest = form->As<RE::TESQuest>();
                    ++quests;
                    running += QuestIsRunning(quest) ? 1 : 0;
                    active += QuestIsActive(quest) ? 1 : 0;
                    if (QuestIsRunning(quest) && QuestShowsObjective(quest)) {
                        ++shown;
                        if (activeIds.size() < 200) activeIds += std::format(" {:08X}{}", id, QuestIsActive(quest) ? "*" : "");
                    }
                } else if (form->Is(RE::FormType::kWTHS)) {
                    ++weathers;
                    if (weatherIds.size() < 120) weatherIds += std::format(" {:08X}", id);
                }
            }
            log::Info("[world] {} quests, {} running, {} active, {} running with an objective shown:{}", quests, running, active, shown, activeIds);
            log::Info("[world] {} weathers:{}; current {:08X} is {}", weathers, weatherIds, CurrentWeather(), RttiName(RE::TESForm::LookupByID(CurrentWeather())));
            return true;
        }
        if (step.starts_with("@next ")) {
            // "@next <quest>": sets the first stage after the last one done.
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(6), nullptr, 16));
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
            if (!quest) return true;
            auto stages = QuestStages(quest);
            std::ranges::sort(stages);
            std::string line;
            int last = -1, next = -1;
            for (const auto& [index, done] : stages) {
                line += std::format(" {}{}", index, done ? "*" : "");
                if (done) last = index;
            }
            for (const auto& [index, done] : stages) {
                if (!done && index > last) {
                    next = index;
                    break;
                }
            }
            log::Info("[world] {}: running {} shown {} stages{}; setting {}", step, QuestIsRunning(quest), QuestShowsObjective(quest), line, next);
            if (next >= 0) avatars::SetQuestStage(id, next);
            return true;
        }
        if (step.starts_with("@stage ")) {
            // "@stage <quest> <stage>"
            const auto space = step.find(' ', 7);
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(7, space - 7), nullptr, 16));
            const auto stage = std::stoi(step.substr(space + 1));
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
            if (!quest) return true;
            log::Info("[world] {}: running {} active {} already done {}", step, QuestIsRunning(quest), QuestIsActive(quest), StageDone(quest, stage));
            avatars::SetQuestStage(id, stage);
            return true;
        }
        if (step.starts_with("@weather ")) {
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(9), nullptr, 16));
            log::Info("[world] {}: was {:08X}", step, CurrentWeather());
            SetWeather(id);
            return true;
        }
        if (step.starts_with("@lock ")) {
            // "@lock <ref> 0|1"
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(6), nullptr, 16));
            ref = LookupRef(id, 0);
            if (ref) {
                log::Info("[world] {}: lock state was {}", step, LockState(ref));
                avatars::SetLocked(id, step.ends_with("1"));
            }
            return true;
        }
        if (step.starts_with("@show ")) {
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(6), nullptr, 16));
            ref = LookupRef(id, 0);
            if (ref) log::Info("[world] {}: lock {} destruction stage {} destroyed {}", step, LockState(ref), DestructionStage(ref), IsDestroyed(ref));
            return true;
        }
        if (step.starts_with("@damage ")) {
            // "@damage <ref> <amount>"
            const auto space = step.find(' ', 8);
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(8, space - 8), nullptr, 16));
            avatars::DamageObject(id, std::stof(step.substr(space + 1)));
            return true;
        }
        if (step.starts_with("@destructibles")) {
            // "@destructibles [<ref>]": anything breakable in the cell of that reference, or the player's.
            const auto* anchor = step.size() > 15 ? LookupRef(static_cast<std::uint32_t>(std::stoul(step.substr(15), nullptr, 16)), 0) : nullptr;
            const auto* cell = anchor ? anchor->parentCell : nullptr;
            std::size_t breakable = 0, locked = 0;
            std::string breakables, lockeds;
            std::vector<RE::TESObjectREFR*> refs;
            if (cell) {
                refs = CellRefs(cell);
            } else {
                // No anchor: every data-file reference in a loaded cell.
                for (std::uint32_t id = 1; id < 0x400000; ++id) {
                    auto* form = RE::TESForm::LookupByID(id);
                    auto* r = form ? form->As<RE::TESObjectREFR>() : nullptr;
                    if (r && r->parentCell && r->parentCell->IsAttached()) refs.push_back(r);
                }
            }
            for (auto* r : refs) {
                if (r->GetFormType() == RE::FormType::kACHR || IsGone(r)) continue;
                if (DestructionStage(r) >= 0) {
                    ++breakable;
                    if (breakables.size() < 240) breakables += std::format(" {:08X}:{}", r->GetFormID(), DestructionStage(r));
                }
                if (LockState(r) >= 0) {
                    ++locked;
                    if (lockeds.size() < 240) lockeds += std::format(" {:08X}:{}", r->GetFormID(), LockState(r));
                }
            }
            log::Info("[world] {} breakable:{}", breakable, breakables);
            log::Info("[world] {} with locks:{}", locked, lockeds);
            return true;
        }
        if (step == "@hostile" || step == "@kill") {
            // The nearest living NPC that is not a stand-in for a player: attacks the local player,
            // or dies. The authority's engine decides what the others see.
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto origin = player->GetPosition();
            const auto playerBase = player->GetBaseObject();
            RE::TESObjectREFR* best = nullptr;
            float bestDistance = 2500.0f;
            for (const auto [first, last] : { std::pair<std::uint32_t, std::uint32_t>{ 1, 0x400000 }, { 0xFF000000, 0xFF080000 } }) {
                for (std::uint32_t id = first; id < last; ++id) {
                    auto* form = RE::TESForm::LookupByID(id);
                    if (!form || form->GetFormType() != RE::FormType::kACHR) continue;
                    auto* r = static_cast<RE::TESObjectREFR*>(form);
                    if (r == player || IsGone(r) || !r->parentCell->IsAttached() || static_cast<RE::Actor*>(r)->IsDead()) continue;
                    if (playerBase && r->GetBaseObject() == playerBase) continue;  // a remote player's stand-in
                    const auto at = r->GetPosition();
                    const auto d = std::hypot(at.x - origin.x, at.y - origin.y, at.z - origin.z);
                    if (d > 150.0f && d < bestDistance) {
                        bestDistance = d;
                        best = r;
                    }
                }
            }
            if (!best) {
                log::Warn("[combat] {}: nobody within 2500", step);
                return true;
            }
            log::Info("[combat] {}: {:08X} base {:08X} at {:.0f}", step, best->GetFormID(), BaseId(best), bestDistance);
            if (step == "@hostile") {
                avatars::StartCombatWithPlayer(best->GetFormID());
            } else {
                avatars::KillActor(best->GetFormID());
            }
            g_lastTarget = best->GetFormID();
            return true;
        }
        if (step == "@killtarget") {
            // Kills the NPC the last @hostile picked.
            log::Info("[combat] @killtarget {:08X}", g_lastTarget);
            if (g_lastTarget != 0) avatars::KillActor(g_lastTarget);
            return true;
        }
        if (step == "@talk") {
            // Starts a conversation with the nearest named, living NPC, as pressing activate would.
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto origin = player->GetPosition();
            RE::TESObjectREFR* best = nullptr;
            float bestDistance = 1500.0f;
            // Every loaded actor: those who walked in are not in the cell's own list.
            std::vector<RE::TESObjectREFR*> actors;
            for (const auto [first, last] : { std::pair<std::uint32_t, std::uint32_t>{ 1, 0x400000 }, { 0xFF000000, 0xFF080000 } }) {
                for (std::uint32_t id = first; id < last; ++id) {
                    auto* form = RE::TESForm::LookupByID(id);
                    if (form && form->GetFormType() == RE::FormType::kACHR) actors.push_back(static_cast<RE::TESObjectREFR*>(form));
                }
            }
            for (auto* r : actors) {
                if (r == player || IsGone(r) || !r->parentCell->IsAttached()) continue;
                if (r->GetBaseObject() == player->GetBaseObject()) continue;  // a remote player's stand-in
                if (proto::IsRuntimeFormId(r->GetFormID())) continue;        // generated crowd: nothing to say
                auto* actor = static_cast<RE::Actor*>(r);
                if (actor->IsDead()) continue;
                const auto base = r->GetBaseObject();
                auto* full = base ? starfield_cast<RE::TESFullName*>(base.get()) : nullptr;
                const char* name = full ? full->GetFullName() : nullptr;
                if (!name || !*name) continue;
                const auto at = r->GetPosition();
                const auto d = std::hypot(at.x - origin.x, at.y - origin.y, at.z - origin.z);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = r;
                }
            }
            if (!best) {
                log::Warn("[dialogue] @talk: nobody named within 1500");
                return true;
            }
            auto* full = starfield_cast<RE::TESFullName*>(best->GetBaseObject().get());
            log::Info("[dialogue] @talk: {:08X} '{}' at {:.0f}", best->GetFormID(), full->GetFullName(), bestDistance);
            avatars::ActivateAsPlayer(best->GetFormID());
            return true;
        }
        if (step == "@home") {
            avatars::RequestHomeShip(g_homeShip);
            return true;
        }
        if (step.starts_with("@sethome ")) {
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(9), nullptr, 16));
            log::Info("[ship] {}: dispatched {}", step, avatars::RequestSetHomeShip(id, g_homeShip));
            return true;
        }
        if (step.starts_with("@goto ")) {
            // "@goto <name>": the player walks over to the first furniture whose name contains it.
            const auto want = step.substr(6);
            auto* player = RE::PlayerCharacter::GetSingleton();
            for (auto* r : CellRefs(player->parentCell)) {
                const auto base = r->GetBaseObject();
                if (!base || !base->Is(RE::FormType::kFURN)) continue;
                auto* full = starfield_cast<RE::TESFullName*>(base.get());
                const char* n = full ? full->GetFullName() : nullptr;
                if (!n || std::string(n).find(want) == std::string::npos) continue;
                log::Info("[ship] {}: to {:08X} '{}'", step, r->GetFormID(), n);
                avatars::MoveRefTo(kPlayerRef, r->GetFormID());
                return true;
            }
            log::Warn("[ship] {}: nothing by that name here", step);
            return true;
        }
        if (step == "@seats" || step == "@pilot") {
            // Furniture in the player's cell, by name; "@pilot" sits in the one named like a pilot seat.
            auto* player = RE::PlayerCharacter::GetSingleton();
            RE::TESObjectREFR* pilot = nullptr;
            for (auto* r : CellRefs(player->parentCell)) {
                const auto base = r->GetBaseObject();
                if (!base || !base->Is(RE::FormType::kFURN)) continue;
                auto* full = starfield_cast<RE::TESFullName*>(base.get());
                const char* n = full ? full->GetFullName() : nullptr;
                const std::string name = n ? n : "";
                log::Info("[ship] furniture {:08X} base {:08X} '{}'", r->GetFormID(), BaseId(r), name);
                if (!pilot && name.find("ilot") != std::string::npos) pilot = r;
            }
            if (step == "@pilot" && pilot) {
                log::Info("[ship] @pilot: sitting in {:08X}", pilot->GetFormID());
                avatars::ActivateAsPlayer(pilot->GetFormID());
            }
            return true;
        }
        if (step == "@placeship") {
            // A stand-in of the home ship's design, next to it.
            auto* ship = LookupRef(g_homeShip->load(), 0);
            if (!ship) return true;
            // In space the native creates the reference at once (0x1F56A10 -> 0x5A26C0) and only answers
            // the script later, so the new ship is looked for directly as well (@ship lists it).
            log::Info("[ship] @placeship: at {:08X} in cell {:08X}: dispatched {}", ship->GetFormID(), ship->parentCell ? ship->parentCell->GetFormID() : 0,
                      avatars::RequestPlaceShip(ship->GetFormID(), BaseId(ship), g_standIn));
            return true;
        }
        if (step.starts_with("@shiftship ") || step.starts_with("@liftship ")) {
            // "@shiftship <dx>" / "@liftship <dz>": moves the home ship itself, as flying would.
            auto* ship = RE::PlayerCharacter::GetSingleton()->GetSpaceship(true);
            if (!ship && g_homeShip->load() > 2) ship = LookupRef(g_homeShip->load(), 0);
            if (!ship) return true;
            auto at = ship->GetPosition();
            const auto before = at;
            (step.starts_with("@lift") ? at.z : at.x) += std::stof(step.substr(step.find(' ') + 1));
            const auto angle = ship->GetAngle();
            avatars::PlaceRef(ship->GetFormID(), { at.x, at.y, at.z }, { angle.x, angle.y, angle.z });
            log::Info("[ship] {}: from {:.1f} {:.1f} {:.1f}", step, before.x, before.y, before.z);
            return true;
        }
        if (step.starts_with("@moveship ")) {
            // "@moveship <dx>": puts the stand-in that far along x from the home ship, same angle.
            auto* ship = LookupRef(g_homeShip->load(), 0);
            auto* standIn = LookupRef(g_standIn->load(), 0);
            if (!ship || !standIn) {
                log::Warn("[ship] {}: ship {} stand-in {:08X}", step, ship != nullptr, g_standIn->load());
                return true;
            }
            auto at = ship->GetPosition();
            at.x += std::stof(step.substr(10));
            const auto angle = ship->GetAngle();
            avatars::PlaceRef(standIn->GetFormID(), { at.x, at.y, at.z }, { angle.x, angle.y, angle.z });
            return true;
        }
        if (step == "@launch") {
            // What the cockpit menu's Launch button does (TakeoffMenu_Launch, 0x16757D0): for a landed
            // ship it closes the menu and posts the take-off. It only reads and sets the menu's
            // "launched" byte at +560, so a zeroed stand-in serves as the menu.
            using Launch_t = void (*)(void* const*);
            static REL::Relocation<Launch_t> launch{ REL::ID(93833) };
            static std::array<std::byte, 1024> menu{};
            menu.fill(std::byte{ 0 });
            void* const self = menu.data();
            launch(&self);
            log::Info("[ship] @launch: menu flag now {}", static_cast<int>(menu[560]));
            return true;
        }
        if (step == "@takeoff") {
            // SpaceshipReference.TakeOff (0x1F220B0): unless 0xB58D50(ship, 0) holds, 0x21199B0(ship), which
            // has the ship's pilot perform the take-off action.
            auto* ship = LookupRef(g_homeShip->load(), 0);
            if (!ship) return true;
            using Guard_t = bool (*)(RE::TESObjectREFR*, bool);
            using TakeOff_t = std::uint64_t (*)(RE::TESObjectREFR*);
            static REL::Relocation<Guard_t> guard{ REL::ID(63482) };
            static REL::Relocation<TakeOff_t> takeOff{ REL::ID(119907) };
            const bool blocked = guard(ship, false);
            log::Info("[ship] @takeoff {:08X}: guard {}, pilot {:08X}", ship->GetFormID(), blocked, ship->GetSpaceshipPilot() ? ship->GetSpaceshipPilot()->GetFormID() : 0);
            if (!blocked) log::Info("[ship] @takeoff returned {:#x}", takeOff(ship));
            return true;
        }
        if (step == "@board") {
            // The console entry runs only commands without a target ('player.moveto' does nothing).
            log::Info("[ship] @board: dispatched {}", avatars::MoveRefTo(kPlayerRef, g_homeShip->load()));
            return true;
        }
        if (step == "@ship") {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ship = player->GetSpaceship(true);
            log::Info("[ship] request result {}", g_homeShip->load());
            if (!ship && g_homeShip->load() > 2) ship = LookupRef(g_homeShip->load(), 0);
            log::Info("[ship] player cell {:08X} pos {:.1f} {:.1f} {:.1f} in ship {:08X}", player->parentCell ? player->parentCell->GetFormID() : 0, player->GetPosition().x,
                      player->GetPosition().y, player->GetPosition().z, ship ? ship->GetFormID() : 0);
            if (ship) {
                const auto base = ship->GetBaseObject();
                const auto at = ship->GetPosition();
                const auto angle = ship->GetAngle();
                log::Info("[ship] {:08X} base {:08X} type {:#x} {} cell {:08X} worldspace {:08X} pos {:.1f} {:.1f} {:.1f} angle {:.3f} {:.3f} {:.3f} landed {}", ship->GetFormID(),
                          BaseId(ship), base ? static_cast<int>(base->GetFormType()) : 0, base ? RttiName(base.get()) : "?", ship->parentCell ? ship->parentCell->GetFormID() : 0,
                          ship->GetParentWorldSpace() ? ship->GetParentWorldSpace()->GetFormID() : 0, at.x, at.y, at.z, angle.x, angle.y, angle.z, ship->IsSpaceshipLanded());
                log::Info("[ship] in space: ship {} player {}", InSpace(ship), InSpace(player));
            }
            if (auto* standIn = g_standIn->load() > 1 && !avatars::IsSpawnError(g_standIn->load()) ? LookupRef(g_standIn->load(), 0) : nullptr) {
                const auto at = standIn->GetPosition();
                log::Info("[ship] stand-in {:08X} base {:08X} cell {:08X} pos {:.1f} {:.1f} {:.1f} in space {} 3D {}", standIn->GetFormID(), BaseId(standIn),
                          standIn->parentCell ? standIn->parentCell->GetFormID() : 0, at.x, at.y, at.z, InSpace(standIn), avatars::Probe(standIn->GetFormID()).has3D);
            } else if (g_standIn->load() != 0) {
                log::Info("[ship] stand-in result {:#x}", g_standIn->load());
            }
            if (ship) {
                // Other references of the same design: a stand-in the script call has not reported yet.
                for (std::uint32_t id = 0xFF000000; id < 0xFF080000; ++id) {
                    auto* form = RE::TESForm::LookupByID(id);
                    auto* r = form ? form->As<RE::TESObjectREFR>() : nullptr;
                    if (!r || r == ship || BaseId(r) != BaseId(ship)) continue;
                    const auto at = r->GetPosition();
                    log::Info("[ship] same design: {:08X} cell {:08X} pos {:.1f} {:.1f} {:.1f} in space {} disabled {} 3D {}", id, r->parentCell ? r->parentCell->GetFormID() : 0, at.x,
                              at.y, at.z, InSpace(r), r->IsDisabled(), avatars::Probe(id).has3D);
                    if (g_standIn->load() == 0 || avatars::IsSpawnError(g_standIn->load())) g_standIn->store(id);
                }
            }
            return true;
        }
        if (step == "@cell") {
            auto* player = RE::PlayerCharacter::GetSingleton();
            std::map<int, int> types;
            std::size_t gone = 0, nobase = 0;
            const auto refs = CellRefs(player->parentCell);
            for (auto* ref : refs) {
                const auto base = ref->GetBaseObject();
                if (!base) {
                    ++nobase;
                    continue;
                }
                gone += IsGone(ref) ? 1 : 0;
                const auto type = static_cast<int>(base->GetFormType());
                if (++types[type] == 1) log::Info("[world] form type {:#x}: {} e.g. base {:08X} ref {:08X}", type, RttiName(base.get()), base->GetFormID(), ref->GetFormID());
            }
            std::string line;
            for (const auto& [type, n] : types) line += std::format(" {:#x}:{}", type, n);
            log::Info("[world] cell {:08X}: {} refs, {} gone, {} without base, types{}", player->parentCell->GetFormID(), refs.size(), gone, nobase, line);
            return true;
        }
        if (step == "@take") {
            ref = Nearest([](RE::TESObjectREFR* r) { return IsContainer(r); });
            if (ref) {
                log::Info("[world] @take: emptying {:08X} base {:08X} into the player", ref->GetFormID(), BaseId(ref));
                avatars::ContainerTakeAll(ref->GetFormID());
            } else {
                log::Warn("[world] @take: no container in this cell");
            }
            return true;
        }
        if (step.starts_with("@ref ")) {
            const auto id = static_cast<std::uint32_t>(std::stoul(step.substr(5), nullptr, 16));
            ref = LookupRef(id, 0);
            if (ref) {
                const auto base = ref->GetBaseObject();
                log::Info("[world] {}: base {} {:08X} door {} state {} gone {} cell {:08X}", step, base ? RttiName(base.get()) : "?", BaseId(ref), IsDoor(ref),
                          DoorState(ref), IsGone(ref), ref->parentCell ? ref->parentCell->GetFormID() : 0);
                avatars::ActivateAsPlayer(id);
            }
            return true;
        }
        if (step.starts_with("@near ")) {
            // "@near <ref> item|take": the same as @item or @take, in the cell that reference is in.
            const auto space = step.find(' ', 6);
            const auto* anchor = LookupRef(static_cast<std::uint32_t>(std::stoul(step.substr(6, space - 6), nullptr, 16)), 0);
            if (!anchor || !anchor->parentCell) return true;
            const bool take = step.ends_with("take");
            // "keep": not food or aid, which taking by script can use up at once (seen 2026-11-07).
            const bool keep = step.ends_with("keep");
            auto* found = take ? Nearest([](RE::TESObjectREFR* r) { return IsContainer(r); }, anchor->parentCell)
                               : Nearest(
                                     [keep](RE::TESObjectREFR* r) {
                                         if (!IsLooseItem(r)) return false;
                                         const auto base = r->GetBaseObject();
                                         return !keep || (base && !base->Is(RE::FormType::kALCH));
                                     },
                                     anchor->parentCell);
            if (!found) {
                log::Warn("[world] {}: nothing live in that cell", step);
                return true;
            }
            log::Info("[world] {}: {:08X} base {:08X}", step, found->GetFormID(), BaseId(found));
            if (!take) _lastItemBase = BaseId(found);
            if (take) {
                avatars::ContainerTakeAll(found->GetFormID());
            } else {
                avatars::ActivateAsPlayer(found->GetFormID());
            }
            return true;
        }
        if (step.starts_with("@netdrop ")) {
            // "@netdrop <s>": the connection drops as on a network failure, and resumes after that long.
            const auto ms = static_cast<std::uint32_t>(std::stof(step.substr(9)) * 1000);
            if (_worker) _worker->SimulateConnectionLoss(ms);
            log::Info("[world] {}: {}", step, _worker ? "dropping the connection" : "no network");
            return true;
        }
        if (step.starts_with("@holdworld ")) {
            _holdUntil = Clock::now() + std::chrono::milliseconds(static_cast<long long>(std::stof(step.substr(11)) * 1000));
            log::Info("[world] {}: holding incoming world events", step);
            return true;
        }
        if (step.starts_with("@drop ")) {
            // "@drop <item> <count>": the player drops that many, through DropObject.
            const auto space = step.find(' ', 6);
            const auto item = static_cast<std::uint32_t>(std::stoul(step.substr(6, space - 6), nullptr, 16));
            const int count = space == std::string::npos ? 1 : std::stoi(step.substr(space + 1));
            auto result = std::make_shared<std::atomic<std::uint32_t>>(0);
            log::Info("[world] {}: {}", step, avatars::RequestDropFromPlayer(item, count, result) ? "sent" : "refused");
            _dropProbe = result;
            return true;
        }
        if (step == "@takedrop") {
            // The last drop placed here from another player, taken by the local player.
            log::Info("[world] @takedrop: {:08X}", _lastPlacedDrop);
            if (_lastPlacedDrop != 0) avatars::ActivateAsPlayer(_lastPlacedDrop);
            return true;
        }
        if (step.starts_with("@give ")) {
            // "@give <item> <count>": the player gets that many, through AddItem.
            const auto space = step.find(' ', 6);
            const auto item = static_cast<std::uint32_t>(std::stoul(step.substr(6, space - 6), nullptr, 16));
            const int count = space == std::string::npos ? 1 : std::stoi(step.substr(space + 1));
            _lastItemBase = item;
            log::Info("[world] {}: {}", step, avatars::ContainerAdd(kPlayerRef, item, count) ? "sent" : "refused");
            return true;
        }
        if (step == "@count" || step.starts_with("@count ")) {
            // "@count [<item>]": logs how many of that item the player has, by default what '@item' took.
            const auto item = step.size() > 7 ? static_cast<std::uint32_t>(std::stoul(step.substr(7), nullptr, 16)) : _lastItemBase;
            auto result = std::make_shared<std::atomic<std::int32_t>>(-2);
            if (avatars::RequestPlayerItemCount(item, result)) _counts.push_back({ item, result });
            return true;
        }
        if (step.starts_with("@time ")) {
            auto* calendar = Calendar();
            if (!calendar) return true;
            const auto hour = std::stof(step.substr(6));
            calendar->gameDaysPassed->value += (hour - calendar->gameHour->value) / 24.0f;
            calendar->gameHour->value = hour;
            log::Info("[world] {}: day {:.4f} hour {:.3f}", step, calendar->gameDaysPassed->value, calendar->gameHour->value);
            return true;
        }
        if (step == "@item") {
            if (auto* item = Nearest(IsLooseItem)) {
                _lastItemBase = BaseId(item);
                log::Info("[world] @item: {:08X} base {:08X}", item->GetFormID(), _lastItemBase);
            }
        }
        if (step == "@door") {
            ref = Nearest([](RE::TESObjectREFR* r) {
                return IsDoor(r) && (!r->extraDataList || !r->extraDataList->HasType(RE::ExtraDataType::kTeleport));
            });
        } else if (step == "@item") {
            ref = Nearest(IsLooseItem);
        } else {
            return false;
        }
        if (!ref) {
            log::Warn("[world] {}: none in this cell", step);
            return true;
        }
        log::Info("[world] {}: activating {:08X} base {:08X} door state {}", step, ref->GetFormID(), BaseId(ref), IsDoor(ref) ? DoorState(ref) : 0);
        avatars::ActivateAsPlayer(ref->GetFormID());
        return true;
    }
}
