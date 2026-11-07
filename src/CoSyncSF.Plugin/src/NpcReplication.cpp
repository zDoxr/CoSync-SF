#include "NpcReplication.h"

#include "AnimEvents.h"
#include "Avatars.h"
#include "MoveControls.h"
#include "NpcSync.h"

#include "cosync/shared/Interpolation.h"
#include "cosync/shared/Log.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace cosync::plugin::npcsync
{
    namespace
    {
        using namespace std::chrono_literals;

        // Engine reads, each taken from the Papyrus native that exposes it (RE 12.1).
        constexpr std::size_t kActorValueOwnerOffset = 0x70;  // +112 in GetValuePercentage 0x20737C0
        constexpr std::size_t kAvGetSlot = 1;                 // current value
        constexpr std::size_t kAvGetPermanentSlot = 2;        // denominator of GetValuePercentage
        constexpr std::size_t kAvModSlot = 7;                 // DamageValue 0x206B300: owner vtbl +56
        constexpr int kAvModifierDamage = 2;
        constexpr std::size_t kIsDeadSlot = 269;              // IsDead 0x1FC7FB0: vtbl +2152
        constexpr std::size_t kIsInCombatSlot = 363;          // GetCombatState 0x1FC6DE0: vtbl +2904
        constexpr std::uint64_t kIsSneakingId = 100547;       // IsSneaking 0x1FD1420 -> 0x18E7660

        constexpr std::size_t kMaxPublished = 300;  // nearest first; the rate budget is checked in NpcBudget tests
        constexpr auto kPublishInterval = 100ms;
        constexpr auto kStaleAfter = 1s;    // then stand still
        constexpr auto kReleaseAfter = 5s;  // then the local AI takes the NPC back

        const RE::ActorValueInfo* HealthAv()
        {
            auto* values = RE::ActorValue::GetSingleton();
            return values ? values->health : nullptr;
        }

        void* Owner(RE::Actor* actor) { return reinterpret_cast<std::byte*>(actor) + kActorValueOwnerOffset; }

        float AvRead(RE::Actor* actor, const RE::ActorValueInfo* av, std::size_t slot)
        {
            auto* owner = Owner(actor);
            const auto vtbl = *reinterpret_cast<void* const* const*>(owner);
            return reinterpret_cast<float (*)(void*, const RE::ActorValueInfo*)>(vtbl[slot])(owner, av);
        }

        // nullopt when the actor has no meaningful health value.
        std::optional<float> HealthPercent(RE::Actor* actor, float* maxOut = nullptr)
        {
            const auto* av = HealthAv();
            if (!av) return std::nullopt;
            const float max = AvRead(actor, av, kAvGetPermanentSlot);
            if (!(max > 0.0f)) return std::nullopt;
            if (maxOut) *maxOut = max;
            return std::clamp(AvRead(actor, av, kAvGetSlot) / max * 100.0f, 0.0f, 100.0f);
        }

        // Same call as Papyrus DamageValue: the damage modifier takes the (negative) delta.
        void ModHealth(RE::Actor* actor, float points)
        {
            const auto* av = HealthAv();
            if (!av) return;
            auto* owner = Owner(actor);
            const auto vtbl = *reinterpret_cast<void* const* const*>(owner);
            reinterpret_cast<void (*)(void*, int, const RE::ActorValueInfo*, float)>(vtbl[kAvModSlot])(
                owner, kAvModifierDamage, av, points);
        }

        template <class R, class... A>
        R CallSlot(RE::Actor* actor, std::size_t slot, A... args)
        {
            const auto vtbl = *reinterpret_cast<void* const* const*>(actor);
            return reinterpret_cast<R (*)(RE::Actor*, A...)>(vtbl[slot])(actor, args...);
        }

        bool IsDead(RE::Actor* actor) { return CallSlot<bool>(actor, kIsDeadSlot, true); }

        template <class T>
        T Field(RE::Actor* actor, std::size_t offset)
        {
            return *reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(actor) + offset);
        }

        bool WeaponDrawn(RE::Actor* actor) { return (Field<std::uint8_t>(actor, 252) & 7u) >= 3; }  // IsWeaponDrawn 0x1FD1490
        bool InScene(RE::Actor* actor) { return (Field<std::uint32_t>(actor, 520) & 8u) != 0; }     // IsInScene 0x1FD1330
        bool Unconscious(RE::Actor* actor)  // SetUnconscious 0x1FCA5F0: life state field
        {
            return (Field<std::uint32_t>(actor, 248) & 0x1E0000u) == 0x60000u;
        }
        bool InDialogueWithPlayer(RE::Actor* actor)  // IsInDialogueWithPlayer 0x20761F0
        {
            return (Field<std::uint32_t>(actor, 892) & 8u) != 0;
        }
        bool Sneaking(RE::Actor* actor)
        {
            static REL::Relocation<bool (*)(RE::Actor*)> fn{ REL::ID(kIsSneakingId) };
            return fn(actor);
        }

        // GetSitState 0x1FC81F0: vfunc 52 of the subobject at +240. 3 = seated, 4 = seated and about
        // to stand. 1 and 2 are NPCs on their way to a seat, still walking.
        bool Sitting(RE::Actor* actor)
        {
            auto* state = reinterpret_cast<std::byte*>(actor) + 240;
            const auto vtbl = *reinterpret_cast<void* const* const*>(state);
            const int sit = reinterpret_cast<int (*)(void*)>(vtbl[52])(state);
            return sit == 3 || sit == 4;
        }

        // Every armor and weapon the actor has equipped, sorted: the walk IsEquipped makes (0xB66510),
        // under the same lock. Runtime forms are left out: their IDs mean nothing on another machine.
        std::vector<std::uint32_t> ReadWorn(RE::Actor* actor)
        {
            std::vector<std::uint32_t> worn;
            actor->ForEachEquippedItem([&](const RE::BGSInventoryItem& item) {
                const auto* object = item.object;
                if (object && (object->Is(RE::FormType::kARMO) || object->Is(RE::FormType::kWEAP)) &&
                    !proto::IsRuntimeFormId(object->GetFormID())) {
                    worn.push_back(object->GetFormID());
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            std::ranges::sort(worn);
            worn.erase(std::ranges::unique(worn).begin(), worn.end());
            if (worn.size() > proto::kMaxNpcOutfitItems) worn.resize(proto::kMaxNpcOutfitItems);
            return worn;
        }

        constexpr std::uint32_t kPlayerBaseId = 0x7;

        // Everything that makes an NPC look the way it does, on the base record (0x3C0-0x430). Equal
        // on two machines means the same face whatever the record's own ID is.
        std::uint32_t LookHash(RE::Actor* actor)
        {
            const auto base = actor->GetBaseObject();
            auto* npc = base ? base->As<RE::TESNPC>() : nullptr;
            if (!npc) return 0;
            std::uint32_t h = 2166136261u;
            const auto byte = [&](std::uint8_t b) { h = (h ^ b) * 16777619u; };
            const auto word = [&](std::uint32_t v) {
                for (int i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(v >> (i * 8)));
            };
            const auto text = [&](const RE::BSFixedString& s) {
                if (const char* p = s.c_str()) {
                    for (; *p; ++p) byte(static_cast<std::uint8_t>(*p));
                }
                byte(0);
            };
            word(npc->IsFemale() ? 1u : 0u);
            if (const auto* race = npc->GetRace()) word(race->GetFormID());
            word(npc->skinToneIndex & 0xFF);  // the rest of the field is uninitialized (2026-11-04)
            text(npc->hairColor);
            text(npc->eyeColor);
            text(npc->facialColor);
            text(npc->eyebrowColor);
            {
                const auto parts = npc->headParts.Lock();
                for (const auto* part : *parts) {
                    if (part) word(part->GetFormID());
                }
            }
            return h == 0 ? 1u : h;
        }

        // Diagnostics: the fields behind LookHash, to compare two machines' logs.
        std::string DescribeLook(RE::Actor* actor)
        {
            const auto base = actor->GetBaseObject();
            auto* npc = base ? base->As<RE::TESNPC>() : nullptr;
            if (!npc) return "no TESNPC";
            const auto* race = npc->GetRace();
            const auto* tmpl = npc->baseTemplateForm;
            auto out = std::format("female {} race {:08X} tmpl {:08X} skin {} hair '{}' eye '{}' facial '{}' brow '{}' parts",
                                   npc->IsFemale() ? 1 : 0, race ? race->GetFormID() : 0u, tmpl ? tmpl->GetFormID() : 0u,
                                   npc->skinToneIndex & 0xFF, npc->hairColor.c_str(), npc->eyeColor.c_str(), npc->facialColor.c_str(),
                                   npc->eyebrowColor.c_str());
            {
                const auto parts = npc->headParts.Lock();
                for (const auto* part : *parts) {
                    if (part) out += std::format(" {:06X}", part->GetFormID());
                }
            }
            float bodySum = 0.0f;
            std::size_t bodyCount = 0;
            if (const auto* body = npc->unk3D8) {
                bodyCount = body->size();
                for (const float v : *body) bodySum += v;
            }
            float shapeSum = 0.0f;
            std::size_t shapeCount = 0;
            if (const auto* shape = npc->shapeBlendData) {
                for (const auto& [name, value] : *shape) {
                    shapeSum += value;
                    ++shapeCount;
                }
            }
            out += std::format(" | body {} {:.3f} tint {} shape {} {:.3f}", bodyCount, bodySum, npc->tintAVMData.size(), shapeCount, shapeSum);
            return out;
        }

        RE::Actor* Lookup(std::uint32_t formId)
        {
            auto* form = RE::TESForm::LookupByID(formId);
            return form && form->Is(RE::FormType::kACHR) ? static_cast<RE::Actor*>(form) : nullptr;
        }

        proto::NpcState ReadState(RE::Actor* actor, std::uint32_t formId)
        {
            proto::NpcState s;
            s.formId = formId;
            const auto p = actor->GetPosition();
            s.position = { p.x, p.y, p.z };
            s.yaw = actor->GetAngle().z;
            const bool dead = IsDead(actor);
            const auto health = HealthPercent(actor);
            s.healthPercent = dead ? 0 : static_cast<std::uint8_t>(std::lround(health.value_or(100.0f)));
            if (dead) s.flags |= proto::kNpcDead;
            if (WeaponDrawn(actor)) s.flags |= proto::kNpcWeaponDrawn;
            if (CallSlot<bool>(actor, kIsInCombatSlot)) s.flags |= proto::kNpcInCombat;
            if (Sneaking(actor)) s.flags |= proto::kNpcSneaking;
            if (InScene(actor)) s.flags |= proto::kNpcInScene;
            if (Unconscious(actor)) s.flags |= proto::kNpcUnconscious;
            if (Sitting(actor)) s.flags |= proto::kNpcSitting;
            return s;
        }
    }

    static_assert(kReplicationSlots == movecontrols::kNpcReplaySlots);

    void Replication::Install(std::string& detail)
    {
        animevents::InstallNpcCapture(detail);
        std::string crowdDetail;
        crowd::Install(crowdDetail);
        log::Info("[crowd] {}", crowdDetail);
    }

    bool Replication::IsProxy(std::uint32_t formId) const { return std::ranges::find(_proxies, formId) != _proxies.end(); }

    void Replication::OnEvents(const proto::NpcEvents& events)
    {
        for (const auto& e : events.events) {
            const auto id = LocalId(e.formId);
            const auto it = _tracked.find(id);
            if (id == 0 || it == _tracked.end() || it->second.slot < 0) continue;
            if (animevents::NotifyLocked(id, e.name.c_str())) ++_eventsApplied;
        }
    }

    void Replication::OnOutfits(const proto::NpcOutfits& outfits)
    {
        if (_isAuthority) return;
        for (const auto& o : outfits.outfits) {
            if (const auto u = _unmatched.find(o.formId); u != _unmatched.end()) {
                u->second.look = o.look;
                u->second.worn = o.items;
            }
            // Only NPCs that passed the identity check in Absorb.
            const auto id = LocalId(o.formId);
            const auto it = _tracked.find(id);
            if (id == 0 || it == _tracked.end()) continue;
            auto& t = it->second;
            t.outfit = o.items;
            t.haveOutfit = true;
            if (!t.lookChecked && o.look != 0) {
                if (auto* actor = Lookup(id)) {
                    t.lookChecked = true;
                    t.lookDiffers = LookHash(actor) != o.look;
                    log::Info("[look] {:08X} {}", id, DescribeLook(actor));
                }
            }
        }
    }

    std::uint32_t Replication::LocalId(std::uint32_t authorityId) const
    {
        if (const auto it = _idMap.find(authorityId); it != _idMap.end()) return it->second;
        // Copies number runtime forms independently: an unpaired number may well be some other
        // actor here, so it means nothing unless Absorb found the same base under it.
        return proto::IsRuntimeFormId(authorityId) && !_sameRuntimeId.contains(authorityId) ? 0 : authorityId;
    }

    // Both copies build their runtime NPCs from the same save but number them differently, so a
    // runtime NPC is paired by its face, the nearest one if several share it.
    void Replication::PairRuntimeNpcs(Clock::time_point now)
    {
        if (now < _nextPairing) return;
        _nextPairing = now + 1s;
        std::erase_if(_unmatched, [&](const auto& u) { return now - u.second.seen > 5s; });
        std::erase_if(_idMap, [&](const auto& m) { return !Lookup(m.second); });
        if (_unmatched.empty()) return;
        auto e = EnumerateHighActors();
        if (!e.ok) return;
        const auto crowd = EnumerateCrowdActors();
        e.actors.insert(e.actors.end(), crowd.begin(), crowd.end());
        std::set<std::uint32_t> taken;
        for (const auto& [from, to] : _idMap) taken.insert(to);
        taken.insert(_sameRuntimeId.begin(), _sameRuntimeId.end());
        struct Candidate
        {
            std::uint32_t id;
            std::uint32_t look;
            std::vector<std::uint32_t> worn;
            Vec3 position;
        };
        std::vector<Candidate> candidates;
        for (const auto& a : e.actors) {
            if (!proto::IsRuntimeFormId(a.formId) || a.baseId == kPlayerBaseId || IsProxy(a.formId) || taken.contains(a.formId)) continue;
            if (auto* actor = Lookup(a.formId)) candidates.push_back({ a.formId, LookHash(actor), ReadWorn(actor), a.position });
        }
        // Crowd actors differ by little more than their clothes, so those decide among equal faces.
        constexpr float kOtherClothes = 10000.0f;
        for (auto it = _unmatched.begin(); it != _unmatched.end();) {
            const auto& [id, u] = *it;
            auto best = candidates.end();
            float bestScore = 0.0f;
            float bestDistance = 0.0f;
            if (u.look != 0) {
                for (auto c = candidates.begin(); c != candidates.end(); ++c) {
                    if (c->look != u.look) continue;
                    const float d = std::hypot(c->position.x - u.position.x, c->position.y - u.position.y, c->position.z - u.position.z);
                    const float score = d + (c->worn == u.worn ? 0.0f : kOtherClothes);
                    if (best == candidates.end() || score < bestScore) {
                        best = c;
                        bestScore = score;
                        bestDistance = d;
                    }
                }
            }
            if (best == candidates.end()) {
                ++it;
                continue;
            }
            if (++_pairs <= 30 || _pairs % 50 == 0) {
                log::Info("[npc] runtime NPC {:08X} is {:08X} here ({:.1f} apart, {})", id, best->id, bestDistance,
                          best->worn == u.worn ? "same clothes" : "other clothes");
            }
            _idMap[id] = best->id;
            candidates.erase(best);
            it = _unmatched.erase(it);
        }
    }

    // An NPC under its local AI, in furniture for example, that is somewhere else entirely.
    void Replication::FollowWhileFree(std::uint32_t formId, Tracked& t, RE::Actor* actor, Clock::time_point now)
    {
        constexpr float kFar = 3.0f;
        constexpr std::uint32_t kMaxSnaps = 3;  // its AI may have good reason: do not fight it forever
        constexpr Clock::time_point kNever{};
        const auto p = actor->GetPosition();
        if (std::hypot(p.x - t.position.x, p.y - t.position.y, p.z - t.position.z) <= kFar) {
            t.bigErrorSince = kNever;
            t.freeSnaps = 0;
            return;
        }
        if (t.bigErrorSince == kNever) t.bigErrorSince = now;
        if (now - t.bigErrorSince < 2s || now < t.nextSnap || t.freeSnaps >= kMaxSnaps) return;
        if (!avatars::Probe(formId).has3D) return;
        t.nextSnap = now + 5s;
        t.bigErrorSince = kNever;
        ++t.freeSnaps;
        avatars::MoveRef(formId, t.position, { 0.0f, 0.0f, t.yaw });
    }

    void Replication::FixOutfit(std::uint32_t formId, Tracked& t, RE::Actor* actor, Clock::time_point now)
    {
        if (!t.haveOutfit || now < t.nextOutfitFix || now < _nextOutfitWork) return;
        t.nextOutfitFix = now + 2s;
        const auto changes = npc::DiffOutfit(t.outfit, ReadWorn(actor));
        t.outfitDiffers = !changes.Empty();
        if (changes.Empty()) {
            t.outfitTries = 0;
            return;
        }
        if (!avatars::Probe(formId).has3D) return;
        // Equipping is a script call, and an NPC's own rules can undo it: back off instead of fighting.
        _nextOutfitWork = now + 50ms;
        t.nextOutfitFix = now + std::chrono::seconds(std::min<std::uint32_t>(2u << std::min(t.outfitTries, 5u), 60u));
        ++t.outfitTries;
        ++_outfitFixes;
        for (const auto item : changes.unequip) avatars::UnequipForm(formId, item);
        for (const auto item : changes.equip) avatars::EquipForm(formId, item);
        if (_outfitFixes <= 40 || _outfitFixes % 100 == 0) {
            log::Info("[npc] {:08X} outfit corrected (try {}): {} put on, {} taken off", formId, t.outfitTries,
                      changes.equip.size(), changes.unequip.size());
        }
    }

    void Replication::PublishOutfits(net::NetworkWorker& worker, const std::vector<proto::NpcState>& states)
    {
        const auto now = Clock::now();
        if (now < _nextOutfits) return;
        _nextOutfits = now + 1s;
        std::vector<proto::NpcOutfit> outfits;
        outfits.reserve(states.size());
        for (const auto& s : states) {
            if (auto* actor = Lookup(s.formId)) {
                outfits.push_back({ s.formId, ReadWorn(actor), LookHash(actor) });
                if (_lookLogged.insert(s.formId).second) log::Info("[look] {:08X} {}", s.formId, DescribeLook(actor));
            }
        }
        ++_outfitsPublished;
        worker.SubmitNpcOutfits(std::move(outfits));
    }

    void Replication::PublishActions(net::NetworkWorker& worker)
    {
        std::vector<animevents::NpcGraphEvent> captured;
        std::uint64_t dropped = 0;
        animevents::DrainNpcEvents(captured, dropped);
        std::vector<proto::NpcEvent> out;
        out.reserve(captured.size());
        for (auto& e : captured) {
            if (IsProxy(e.formId)) continue;
            if (e.name.size() > proto::kMaxAnimEventNameBytes || !proto::IsValidText(e.name, proto::kMaxAnimEventNameBytes)) continue;
            if (_eventNames.size() < 512 || _eventNames.contains(e.name)) ++_eventNames[e.name];
            out.push_back({ e.formId, std::move(e.name) });
        }
        _eventsPublished += out.size();
        if (!out.empty()) worker.SubmitNpcEvents(std::move(out));
    }

    void Replication::OnAuthority(PlayerId authority)
    {
        _authority = authority;
        log::Info("[npc] authority is player {}", authority);
    }

    void Replication::Reset(const char* why)
    {
        ReleaseAll(why);
        crowd::SetInhibitionOff(false);
        _authority = kInvalidPlayerId;
        _isAuthority = false;
        animevents::SetNpcCapture(false);
        _dead.clear();
        _killsApplied.clear();
        _unmatched.clear();
        _idMap.clear();
        _sameRuntimeId.clear();
        _baseDiffers.clear();
    }

    void Replication::OnDeath(std::uint32_t formId)
    {
        _dead.insert(formId);
        _nextDeathCheck = {};
    }

    void Replication::OnDamage(const proto::NpcDamage& damage)
    {
        if (!_isAuthority) return;
        auto* actor = Lookup(damage.npcFormId);
        if (!actor || IsDead(actor)) return;
        const bool ok = avatars::DamageHealth(damage.npcFormId, damage.amount);
        log::Info("[npc] player {} hit {:08X} for {:.1f}: {}", damage.playerId, damage.npcFormId, damage.amount,
                  ok ? "applied" : "refused");
    }

    void Replication::ReleaseAll(const char* why)
    {
        for (auto& [formId, t] : _tracked) {
            if (t.slot >= 0) Release(formId, t, why);
        }
        _tracked.clear();
        _crowdOrders.clear();
        crowd::SetOrders(_crowdOrders);
    }

    void Replication::Tick(net::NetworkWorker& worker, const Context& ctx)
    {
        if (!ctx.inWorld || ctx.locationId == 0) {
            if (!_tracked.empty()) ReleaseAll("not in the world");
            return;
        }
        _proxies = ctx.proxies;
        // Derived here, not in OnAuthority: the local id may not be known yet when that event arrives.
        const bool mine = _authority != kInvalidPlayerId && _authority == ctx.self;
        if (mine != _isAuthority) {
            _isAuthority = mine;
            _nextPublish = {};
            if (mine) ReleaseAll("this client became the NPC authority");
            animevents::SetNpcCapture(mine);
            log::Info("[npc] this client {} the NPC authority", mine ? "is now" : "is no longer");
        }
        if (!ctx.vmSettled || _authority == kInvalidPlayerId) return;
        crowd::SetInhibitionOff(true);
        if (_isAuthority) {
            TickAuthority(worker, ctx);
        } else {
            TickReplica(worker, ctx);
        }
        LogSummary(ctx);
    }

    void Replication::TickAuthority(net::NetworkWorker& worker, const Context& ctx)
    {
        PublishActions(worker);
        const auto now = Clock::now();
        if (now < _nextPublish) return;
        _nextPublish = now + kPublishInterval;
        auto e = EnumerateHighActors();
        if (!e.ok) return;
        if (now >= _nextExtraScan) {
            _nextExtraScan = now + 1s;
            _extraActors = EnumerateOtherLoadedActors();
        }
        e.actors.insert(e.actors.end(), _extraActors.begin(), _extraActors.end());
        // The ambient crowd is CrowdManager's, not in the lists above (RE 12.10).
        const auto crowd = EnumerateCrowdActors();
        e.actors.insert(e.actors.end(), crowd.begin(), crowd.end());
        _lastCrowd = crowd.size();
        _lastEnumerated = e.actors.size();

        std::vector<proto::NpcState> states;
        states.reserve(e.actors.size());
        std::size_t runtime = 0;
        for (const auto& a : e.actors) {
            if (!proto::IsReplicatedNpcId(a.formId) || IsProxy(a.formId)) continue;
            auto* actor = Lookup(a.formId);
            if (!actor) continue;
            auto s = ReadState(actor, a.formId);
            s.baseId = a.baseId;
            if (a.hasCrowdTarget) {
                s.flags |= proto::kNpcCrowdTarget;
                s.crowdTarget = a.crowdTarget;
            }
            if (s.baseId == kPlayerBaseId) continue;  // a copy of a player, never an NPC
            if (!proto::IsValidNpcState(s)) {
                if (_invalidLogged.insert(a.formId).second) {
                    log::Warn("[npc] {:08X} refused: base {:08X} pos {:.1f} {:.1f} {:.1f} yaw {} health {} flags {:#x}", a.formId, s.baseId,
                              s.position.x, s.position.y, s.position.z, s.yaw, s.healthPercent, s.flags);
                }
                continue;
            }
            if (proto::IsRuntimeFormId(a.formId)) ++runtime;
            if ((s.flags & proto::kNpcDead) != 0 && _dead.insert(a.formId).second) {
                worker.SubmitNpcDeath(a.formId);
                log::Info("[npc] {:08X} died here; announced", a.formId);
            }
            states.push_back(s);
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto p = player ? player->GetPosition() : RE::NiPoint3{};
        npc::KeepNearest(states, { p.x, p.y, p.z }, kMaxPublished);
        _lastPublishedCount = states.size();
        _lastPublishedRuntime = runtime;
        ++_published;
        PublishOutfits(worker, states);
        worker.SubmitNpcStates(ctx.locationId, std::move(states));
    }

    void Replication::Absorb(const proto::NpcStates& states)
    {
        const auto now = Clock::now();
        ++_received;
        for (const auto& s : states.npcs) {
            auto id = LocalId(s.formId);
            if (id == 0) {
                // A runtime reference is the same NPC only if this machine's has the same base;
                // otherwise PairRuntimeNpcs looks for it.
                const auto* same = IsProxy(s.formId) ? nullptr : Lookup(s.formId);
                const auto sameBaseObject = same ? same->GetBaseObject() : nullptr;
                if (!sameBaseObject || sameBaseObject->GetFormID() != s.baseId) {
                    auto& u = _unmatched[s.formId];
                    u.base = s.baseId;
                    u.position = s.position;
                    u.seen = now;
                    ++_tallyUnmatched;
                    continue;
                }
                _sameRuntimeId.insert(s.formId);
                _unmatched.erase(s.formId);
                id = s.formId;
            }
            if (IsProxy(id)) {
                ++_tallyProxy;
                continue;
            }
            ++(id == s.formId ? _tallySameId : _tallyPaired);
            auto* local = Lookup(id);
            const auto base = local ? local->GetBaseObject() : nullptr;
            const bool sameBase = base && base->GetFormID() == s.baseId;
            if (local && !sameBase && !proto::IsRuntimeFormId(s.baseId)) {
                // Generated bases are numbered per copy, so only data-file ones can be compared.
                _baseDiffers[s.formId] = s.baseId;
            }
            auto [it, inserted] = _tracked.try_emplace(id);
            auto& t = it->second;
            if (!inserted) {
                const float dt = std::chrono::duration<float>(now - t.receivedAt).count();
                const float dx = s.position.x - t.position.x, dy = s.position.y - t.position.y,
                            dz = s.position.z - t.position.z;
                constexpr float kTeleport = 20.0f;
                if (dt < 0.02f) continue;  // same frame burst: keep the newer velocity estimate
                if (std::sqrt(dx * dx + dy * dy + dz * dz) > kTeleport) {
                    t.velX = t.velY = t.velZ = 0.0f;
                } else {
                    t.velX = dx / dt;
                    t.velY = dy / dt;
                    t.velZ = dz / dt;
                }
            }
            t.position = s.position;
            t.yaw = s.yaw;
            t.healthPercent = s.healthPercent;
            t.flags = s.flags;
            if ((s.flags & proto::kNpcCrowdTarget) != 0) {
                const bool moved = !t.hasCrowdTarget || t.crowdTarget.x != s.crowdTarget.x || t.crowdTarget.y != s.crowdTarget.y ||
                                   t.crowdTarget.z != s.crowdTarget.z;
                if (moved) t.crowdGeneration = 0;  // look it up again
                t.hasCrowdTarget = true;
                t.crowdTarget = s.crowdTarget;
            }
            t.receivedAt = now;
            t.fresh = true;
        }
    }

    void Replication::TickReplica(net::NetworkWorker& worker, const Context& ctx)
    {
        for (const auto& states : worker.TakeNpcStates()) {
            // A different location is no reason to skip: NPCs across a cell border are loaded on
            // both machines. Whether an NPC is loaded here decides, per NPC, in UpdateOne.
            if (states.locationId != ctx.locationId) ++_otherLocation;
            Absorb(states);
        }

        const auto now = Clock::now();
        RefreshCrowd(now);
        _crowdOrders.clear();
        for (auto it = _tracked.begin(); it != _tracked.end();) {
            auto& [formId, t] = *it;
            if (now - t.receivedAt > kReleaseAfter) {
                if (t.slot >= 0) Release(formId, t, "no state from the authority");
                it = _tracked.erase(it);
                continue;
            }
            UpdateOne(formId, t, worker);
            t.fresh = false;
            ++it;
        }
        crowd::SetOrders(_crowdOrders);
        ApplyPendingDeaths();
        PairRuntimeNpcs(now);

        // The engine's own input dispatch crashes on dead entries in PlayerControls::Manager
        // (RE A.10 / crash 0x12C70D8). Only data-file references are taken over, and those are not
        // deleted at runtime, but the entries cost nothing to keep clean.
        if (now >= _nextPurge) {
            _nextPurge = now + 1s;
            movecontrols::PurgeFromPlayerControls(0);
        }
    }

    void Replication::RefreshCrowd(Clock::time_point now)
    {
        if (now < _nextCrowdRefresh || !crowd::Installed()) return;
        _nextCrowdRefresh = now + 1s;
        _localCrowd.clear();
        for (const auto& a : EnumerateCrowdActors()) _localCrowd.insert(a.formId);
        _crowdTargets = EnumerateCrowdTargets();
        if (++_crowdGeneration == 0) _crowdGeneration = 1;  // 0 means "look it up"
    }

    // A crowd actor whose target exists here: the engine walks and seats it, the authority only
    // chooses where. Anything else stays on the driven path below.
    bool Replication::FollowsCrowd(std::uint32_t formId, Tracked& t)
    {
        if (!t.hasCrowdTarget || !crowd::Installed() || !_localCrowd.contains(formId)) return false;
        if (t.crowdGeneration != _crowdGeneration) {
            t.crowdGeneration = _crowdGeneration;
            t.crowdHandle = npc::FindCrowdTarget(_crowdTargets, t.crowdTarget);
        }
        if (t.crowdHandle == 0) ++_crowdNoTarget;
        return t.crowdHandle != 0;
    }

    void Replication::UpdateOne(std::uint32_t formId, Tracked& t, net::NetworkWorker& worker)
    {
        const auto now = Clock::now();
        auto* actor = Lookup(formId);
        if (!actor) {
            if (t.slot >= 0) Release(formId, t, "actor gone");
            return;
        }
        if (IsDead(actor) || _dead.contains(formId)) {
            if (t.slot >= 0) Release(formId, t, "dead");
            return;
        }

        FixOutfit(formId, t, actor, now);

        if (t.fresh) {
            float max = 0.0f;
            if (const auto local = HealthPercent(actor, &max)) {
                if (const float drop = t.health.ObserveLocal(*local); drop > 0.0f) {
                    worker.SubmitNpcDamage(formId, drop / 100.0f * max);
                    ++_damageForwarded;
                }
                if (const auto change = t.health.OnAuthoritative(t.healthPercent, *local)) {
                    ModHealth(actor, *change / 100.0f * max);
                }
            }
            const bool drawn = (t.flags & proto::kNpcWeaponDrawn) != 0;
            if (drawn != WeaponDrawn(actor) && now >= t.nextDraw) {
                t.nextDraw = now + 1s;
                avatars::SetWeaponDrawn(formId, drawn);
            }
        }

        // Its conversation with this client's player needs the local actor, in any mode.
        if (!InDialogueWithPlayer(actor) && FollowsCrowd(formId, t)) {
            if (t.slot >= 0) Release(formId, t, "crowd: following the authority's targets");
            _crowdOrders.push_back({ formId, t.crowdHandle });
            FollowWhileFree(formId, t, actor, now);
            return;
        }

        // In furniture on the authority: a held NPC cannot sit, so its local AI gets it back here.
        // Stationary too: a moving NPC stays driven whatever its flags say.
        if ((t.flags & proto::kNpcSitting) != 0 && std::hypot(t.velX, t.velY) < 0.2f) {
            if (t.slot >= 0) Release(formId, t, "sitting on the authority");
            t.nextAttempt = now + 1s;
            FollowWhileFree(formId, t, actor, now);
            return;
        }
        // This client's player talking to the NPC: dialogue here needs the local actor.
        if (InDialogueWithPlayer(actor)) {
            if (t.slot >= 0) Release(formId, t, "in dialogue with this client's player");
            t.nextAttempt = now + 2s;
            return;
        }
        if (t.slot < 0) {
            if (now >= t.nextAttempt && now - t.receivedAt < kStaleAfter && !TakeOver(formId, t)) {
                FollowWhileFree(formId, t, actor, now);
            }
            return;
        }
        if (now >= t.nextLoadCheck) {
            t.nextLoadCheck = now + 1s;
            if (!avatars::Probe(formId).has3D) {
                Release(formId, t, "unloaded here");
                return;
            }
        }
        // Held controllers refuse state changes, so this means a path the hold does not cover.
        if (movecontrols::ControllerState(formId) != 1) {
            ++t.lostControl;
            Release(formId, t, "controller left the player-controls state");
            t.nextAttempt = now + 2s;
            return;
        }
        Drive(formId, t, now);
    }

    bool Replication::TakeOver(std::uint32_t formId, Tracked& t)
    {
        const auto now = Clock::now();
        t.nextAttempt = now + 2s;
        if (!avatars::Probe(formId).has3D) return false;
        // Only an NPC in the plain motion-driven AI state: anything else (furniture, animation-driven)
        // is left to the local engine.
        // State 1 with no slot of ours: the engine put it there (seen 2026-11-04), and it steers the
        // same. State 0: crowd actors between crowd moves.
        const auto controllerState = movecontrols::ControllerState(formId);
        if ((controllerState != 2 && controllerState != 1 && controllerState != 0) || movecontrols::ControllerMode(formId) != 2) {
            ++_skippedBusy;
            if (_busyLogged.insert(formId).second) {
                log::Info("[npc] {:08X} not taken over: controller state {} mode {}", formId,
                          movecontrols::ControllerState(formId), movecontrols::ControllerMode(formId));
            }
            return false;
        }
        const auto free = std::ranges::find(_slotUsed, false);
        if (free == _slotUsed.end()) return false;
        const int slot = static_cast<int>(free - _slotUsed.begin());
        const auto replay = movecontrols::kNpcSlotBase + static_cast<std::uint32_t>(slot);
        std::string detail, state;
        const bool attached = movecontrols::AttachReplay(formId, replay, detail);
        const bool entered = attached && (controllerState == 1 || movecontrols::EnterPlayerControlsState(formId, state));
        log::Info("[npc] take over {:08X} slot {}: {} | {}", formId, slot, detail, state);
        if (!entered) {
            std::string undo;
            movecontrols::LeavePlayerControlsState(formId, undo);
            return false;
        }
        *free = true;
        t.slot = slot;
        if (!movecontrols::SetControllerHeld(formId, true)) log::Warn("[npc] {:08X}: controller not held", formId);
        animevents::SetGraphLocked(formId, true);
        return true;
    }

    void Replication::Drive(std::uint32_t formId, Tracked& t, Clock::time_point now)
    {
        const auto replay = movecontrols::kNpcSlotBase + static_cast<std::uint32_t>(t.slot);
        const bool stale = now - t.receivedAt > kStaleAfter;
        movecontrols::SetReplay(replay, {}, !stale);
        if (stale) return;
        auto* actor = Lookup(formId);
        if (!actor) return;

        const float age = std::clamp(std::chrono::duration<float>(now - t.receivedAt).count(), 0.0f, 0.25f);
        const Vec3 target{ t.position.x + t.velX * age, t.position.y + t.velY * age, t.position.z + t.velZ * age };
        const auto p = actor->GetPosition();
        const Vec3 error{ target.x - p.x, target.y - p.y, target.z - p.z };
        const float gap = std::sqrt(error.x * error.x + error.y * error.y + error.z * error.z);

        const float yaw = actor->GetAngle().z;
        const auto steer = npc::Steer(error, t.velX, t.velY, yaw);
        movecontrols::SetReplayMove(replay, steer.angle, steer.magnitude);

        // Same turn law as a remote player's proxy (RE A.10: +0x18 is a turn rate).
        constexpr float kTurnGain = 10.0f;
        constexpr float kMaxTurnRate = 3.0f;
        const float yawError = TransformInterpolator::AngleDelta(yaw, t.yaw);
        movecontrols::SetReplayTurn(replay, std::clamp(yawError * kTurnGain, -kMaxTurnRate, kMaxTurnRate), yaw + yawError);

        constexpr Clock::time_point kNever{};
        constexpr float kSnapDistance = 3.0f;
        if (gap <= kSnapDistance) {
            t.bigErrorSince = kNever;
        } else if (t.bigErrorSince == kNever) {
            t.bigErrorSince = now;
        }
        if (t.bigErrorSince != kNever && now - t.bigErrorSince > 400ms && now >= t.nextSnap) {
            t.nextSnap = now + 1s;
            t.bigErrorSince = kNever;
            avatars::MoveRef(formId, target, { 0.0f, 0.0f, t.yaw });
            if (++t.snaps <= 5 || t.snaps % 50 == 0) {
                log::Info("[npc] {:08X} snapped {:.2f} to the authority's position (#{})", formId, gap, t.snaps);
            }
        }
    }

    void Replication::Release(std::uint32_t formId, Tracked& t, const char* why)
    {
        const auto replay = movecontrols::kNpcSlotBase + static_cast<std::uint32_t>(t.slot);
        movecontrols::SetReplay(replay, {}, false);
        animevents::SetGraphLocked(formId, false);
        std::string detail;
        const bool ok = movecontrols::LeavePlayerControlsState(formId, detail);
        movecontrols::PurgeFromPlayerControls(formId);
        log::Info("[npc] release {:08X} ({}): {}{}", formId, why, detail, ok ? "" : " (not restored)");
        _slotUsed[static_cast<std::size_t>(t.slot)] = false;
        t.slot = -1;
        t.bigErrorSince = {};
    }

    void Replication::ApplyPendingDeaths()
    {
        const auto now = Clock::now();
        if (now < _nextDeathCheck || _dead.size() == _killsApplied.size()) return;
        _nextDeathCheck = now + 1s;
        // Only loaded NPCs: what the engine does to an unloaded actor is not known (status doc).
        const auto e = EnumerateHighActors();
        if (!e.ok) return;
        for (const auto& a : e.actors) {
            if (!_dead.contains(a.formId) || _killsApplied.contains(a.formId)) continue;
            auto* actor = Lookup(a.formId);
            if (!actor) continue;
            if (IsDead(actor)) {
                _killsApplied.insert(a.formId);
                continue;
            }
            if (auto it = _tracked.find(a.formId); it != _tracked.end() && it->second.slot >= 0) {
                Release(a.formId, it->second, "dead");
            }
            const bool ok = avatars::KillSilent(a.formId);
            log::Info("[npc] {:08X} died on the authority: KillSilent {}", a.formId, ok ? "dispatched" : "failed");
            if (ok) _killsApplied.insert(a.formId);
        }
    }

    void Replication::ScanLists()
    {
        const auto now = Clock::now();
        if (_listScans >= 6 || now < _nextListScan) return;
        ++_listScans;
        _nextListScan = now + 30s;
        log::Info("[npc] actor lists:{}", ScanActorLists());
    }

    void Replication::LogSummary(const Context& ctx)
    {
        ScanLists();
        const auto now = Clock::now();
        if (now >= _nextNearLog) {
            // Diagnostics: who stands around this client's player, to compare the two machines.
            _nextNearLog = now + 5s;
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto e = EnumerateHighActors();
            if (player && e.ok) {
                const auto me = player->GetPosition();
                std::string line;
                for (const auto& a : e.actors) {
                    const float d = std::hypot(a.position.x - me.x, a.position.y - me.y, a.position.z - me.z);
                    if (d < 15.0f) line += std::format(" {:08X}:{:.1f}/{:.1f}/{:.1f}", a.formId, a.position.x, a.position.y, a.position.z);
                }
                log::Info("[near] me {:.1f}/{:.1f}:{}", me.x, me.y, line);
            }
        }
        if (now >= _nextCrowdLog) {
            _nextCrowdLog = now + 5s;
            const auto crowd = EnumerateCrowdActors();
            std::size_t with3D = 0, runtimeBase = 0;
            const auto high = EnumerateHighActors();
            std::size_t inHigh = 0;
            for (const auto& a : crowd) {
                if (avatars::Probe(a.formId).has3D) ++with3D;
                if (proto::IsRuntimeFormId(a.baseId)) ++runtimeBase;
                if (std::ranges::any_of(high.actors, [&](const ActorInfo& h) { return h.formId == a.formId; })) ++inHigh;
                if (_crowdLogged.insert(a.formId).second && _crowdLogged.size() < 120) {
                    auto* actor = Lookup(a.formId);
                    if (!actor) continue;
                    std::string worn;
                    for (const auto item : ReadWorn(actor)) worn += std::format(" {:06X}", item);
                    log::Info("[crowd] {:08X} base {:08X} at {:.1f} {:.1f} {:.1f} look {:08X} {} | worn{}", a.formId, a.baseId, a.position.x,
                              a.position.y, a.position.z, LookHash(actor), DescribeLook(actor), worn);
                }
            }
            log::Info("[crowd] {} actors, {} with 3D, {} with a generated base, {} also in the actor list", crowd.size(), with3D, runtimeBase, inHigh);
            const auto counters = crowd::TakeCounters();
            log::Info("[crowd] {} targets known, {} following now, {} checks without a local target; in 5 s {} retargets, {} decisions held",
                      _crowdTargets.size(), _crowdOrders.size(), _crowdNoTarget, counters.retargets, counters.holds);
            _crowdNoTarget = 0;
        }
        if (now < _nextLog) return;
        _nextLog = now + 10s;
        if (_isAuthority) {
            log::Info("[npc] authority: {} snapshots published, {} of {} loaded NPCs in the last one ({} runtime, {} crowd, {} "
                      "from other lists), {} graph events, {} outfit lists, {} deaths known",
                      _published, _lastPublishedCount, _lastEnumerated, _lastPublishedRuntime, _lastCrowd, _extraActors.size(),
                      _eventsPublished, _outfitsPublished, _dead.size());
            if (now >= _nextUnmatchedLog) {
                _nextUnmatchedLog = now + 30s;
                std::vector<std::pair<std::uint32_t, std::string>> top;
                for (const auto& [name, count] : _eventNames) top.emplace_back(count, name);
                std::ranges::sort(top, std::greater{});
                std::string line;
                for (std::size_t i = 0; i < top.size() && i < 24; ++i) line += std::format(" {}={}", top[i].second, top[i].first);
                log::Info("[npc] graph events seen:{}", line);
            }
            return;
        }
        std::size_t taken = 0, lost = 0, snaps = 0;
        for (const auto& [id, t] : _tracked) {
            if (t.slot >= 0) ++taken;
            lost += t.lostControl;
            snaps += t.snaps;
        }
        log::Info("[npc] replica: {} parts ({} from another location), {} tracked, {} taken over, {} runtime NPCs "
                  "unmatched, {} busy skips, {} control losses, {} snaps, {} hits forwarded, location {:08X}",
                  _received, _otherLocation, _tracked.size(), taken, _unmatched.size(), _skippedBusy, lost, snaps,
                  _damageForwarded, ctx.locationId);
        // How well each NPC this machine has loaded matches the authority's.
        std::size_t within50cm = 0, within2m = 0, further = 0, sitting = 0, freeRunning = 0, outfitDiffers = 0, outfitKnown = 0;
        std::size_t looksChecked = 0, looksDiffer = 0, held = 0, seatedBoth = 0, seatedOne = 0;
        std::string offenders;
        float worst = 0.0f;
        std::uint32_t worstId = 0;
        std::string otherLook;
        for (const auto& [id, t] : _tracked) {
            if (t.lookChecked) {
                ++looksChecked;
                if (t.lookDiffers) {
                    ++looksDiffer;
                    if (otherLook.size() < 200) otherLook += std::format(" {:08X}", id);
                }
            }
            if (t.haveOutfit) {
                ++outfitKnown;
                if (t.outfitDiffers) ++outfitDiffers;
            }
            auto* actor = Lookup(id);
            if (!actor || !avatars::Probe(id).has3D) continue;
            const auto p = actor->GetPosition();
            const float gap = std::hypot(p.x - t.position.x, p.y - t.position.y, p.z - t.position.z);
            if (gap > worst) {
                worst = gap;
                worstId = id;
            }
            const bool seatedThere = (t.flags & proto::kNpcSitting) != 0;
            const bool seatedHere = Sitting(actor);
            if (seatedThere && seatedHere) ++seatedBoth;
            if (seatedThere != seatedHere) ++seatedOne;
            if (gap >= 2.0f && offenders.size() < 420) {
                offenders += std::format(" {:08X}:{:.1f}/{}/{}{}/c{}/v{:.1f}/s{}", id, gap, t.slot >= 0 ? "held" : "free", seatedThere ? 'T' : 'f',
                                         seatedHere ? 'H' : 'f', movecontrols::ControllerState(id), std::hypot(t.velX, t.velY), t.freeSnaps);
            }
            if (gap < 0.5f) ++within50cm;
            else if (gap < 2.0f) ++within2m;
            else ++further;
            if ((t.flags & proto::kNpcSitting) != 0) ++sitting;
            else if (t.slot < 0) ++freeRunning;
            if (t.slot >= 0) ++held;
        }
        log::Info("[npc] accuracy: {} within 0.5, {} within 2, {} further, worst {:.1f} ({:08X}); {} held, {} sitting on the "
                  "authority, {} loaded but not held; outfits {} known, {} differ, {} corrections; looks {} checked, {} differ; "
                  "{} runtime NPCs paired; {} same-id NPCs with another base",
                  within50cm, within2m, further, worst, worstId, held, sitting, freeRunning, outfitKnown, outfitDiffers, _outfitFixes,
                  looksChecked, looksDiffer, _pairs, _baseDiffers.size());
        if (!otherLook.empty()) log::Info("[npc] look differs:{}", otherLook);
        log::Info("[npc] seated on both {}, on one side only {}; far:{}", seatedBoth, seatedOne, offenders);
        log::Info("[npc] graph: {} events applied, {} local events blocked", _eventsApplied, animevents::BlockedLocalEvents());
        log::Info("[npc] received per 10 s: {} same id, {} paired, {} unmatched, {} proxies; {} pairs held, {} same-id runtime",
                  _tallySameId, _tallyPaired, _tallyUnmatched, _tallyProxy, _idMap.size(), _sameRuntimeId.size());
        _tallySameId = _tallyPaired = _tallyUnmatched = _tallyProxy = 0;
        if (now >= _nextBlockedLog) {
            _nextBlockedLog = now + 30s;
            std::vector<std::pair<std::uint32_t, std::string>> top;
            for (const auto& [name, count] : animevents::BlockedEventNames()) top.emplace_back(count, name);
            std::ranges::sort(top, std::greater{});
            std::string line;
            for (std::size_t i = 0; i < top.size() && i < 24; ++i) line += std::format(" {}={}", top[i].second, top[i].first);
            log::Info("[npc] local events blocked:{}", line);
            line.clear();
            for (const auto& [id, base] : _baseDiffers) {
                const auto* actor = Lookup(id);
                const auto local = actor ? actor->GetBaseObject() : nullptr;
                line += std::format(" {:08X}:{:08X}/{:08X}", id, base, local ? local->GetFormID() : 0u);
            }
            if (!line.empty()) log::Info("[npc] other base (id:authority/here):{}", line.substr(0, 600));
        }
        if (now >= _nextUnmatchedLog && !_unmatched.empty()) {
            _nextUnmatchedLog = now + 30s;
            std::string line;
            for (const auto& [id, u] : _unmatched) line += std::format(" {:08X}/{:08X}", id, u.base);
            log::Info("[npc] unmatched runtime NPCs (id/base):{}", line.substr(0, 400));
        }
        log::Info("[npc] state changes refused: default {} s8 {} s10 {} s11 {} s12 {} s13 {} s14 {}",
                  movecontrols::HoldRefusals(7), movecontrols::HoldRefusals(8), movecontrols::HoldRefusals(10),
                  movecontrols::HoldRefusals(11), movecontrols::HoldRefusals(12), movecontrols::HoldRefusals(13),
                  movecontrols::HoldRefusals(14));
    }
}
