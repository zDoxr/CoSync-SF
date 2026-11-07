// Engine/VM code for remote-player avatars. See Avatars.h for the verified
// Papyrus signatures this relies on.

#include "Avatars.h"

#include "cosync/shared/Log.h"

#include <SFSE/SFSE.h>

#include <RE/Starfield.h>

#include <bit>
#include <cmath>
#include <limits>
#include <format>
#include <functional>
#include <numbers>

namespace cosync::plugin::avatars
{
    namespace
    {
        using RE::BSScript::Variable;
        using ArgsFn = std::function<void(RE::BSScrapArray<Variable>&)>;

        // Result callback for one VM call.
        class Callback final : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            explicit Callback(std::function<void(const Variable&)> fn, std::function<void()> onCancel = {}) :
                _fn(std::move(fn)), _onCancel(std::move(onCancel))
            {}
            void CallQueued() override {}
            void CallCanceled() override
            {
                if (_onCancel) _onCancel();
            }
            void StartMultiDispatch() override {}
            void EndMultiDispatch() override {}
            void operator()(Variable result) override
            {
                if (_fn) _fn(result);
            }

        private:
            std::function<void(const Variable&)> _fn;
            std::function<void()> _onCancel;
        };

        RE::BSScript::IVirtualMachine* VM()
        {
            const auto game = RE::GameVM::GetSingleton();
            return game ? game->GetVM() : nullptr;
        }

        // Script object for a form (finds or creates + binds it, CommonLibSF PackVariable).
        template <class T>
        RE::BSTSmartPointer<RE::BSScript::Object> ScriptObjectFor(T* form)
        {
            Variable v;
            RE::BSScript::PackVariable(v, form);
            if (!v.is<RE::BSScript::Object>()) {
                return nullptr;
            }
            return RE::BSScript::get<RE::BSScript::Object>(v);
        }

        bool Dispatch(const RE::BSTSmartPointer<RE::BSScript::Object>& self, const char* function, ArgsFn args,
                      std::function<void(const Variable&)> onResult = {}, std::function<void()> onCancel = {})
        {
            auto* vm = VM();
            if (!vm || !self) {
                return false;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            if (onResult) {
                callback.reset(new Callback(std::move(onResult), std::move(onCancel)));
            }
            const RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<Variable>&)> packer =
                [args = std::move(args)](RE::BSScrapArray<Variable>& a) {
                    args(a);
                    return true;
                };
            return vm->DispatchMethodCall(self, function, packer, callback, 0);
        }

        RE::Actor* LookupActor(std::uint32_t refId)
        {
            auto* form = RE::TESForm::LookupByID(refId);
            if (!form || !form->Is(RE::FormType::kACHR)) {
                return nullptr;
            }
            return static_cast<RE::Actor*>(form);
        }

        template <class T>
        void Push(RE::BSScrapArray<Variable>& a, T value)
        {
            a.emplace_back();
            RE::BSScript::PackVariable(a.back(), value);
        }

        void PushNone(RE::BSScrapArray<Variable>& a)
        {
            a.emplace_back();
            a.back() = nullptr;
        }

        float Degrees(float radians) { return radians * 180.0f / std::numbers::pi_v<float>; }

        // "ActorBase -> Form" style chain of a script object's type, for diagnostics.
        std::string TypeChain(const RE::BSTSmartPointer<RE::BSScript::Object>& object)
        {
            if (!object) return "<null object>";
            std::string chain;
            const RE::BSScript::ObjectTypeInfo* type = object->type.get();
            for (int depth = 0; type && depth < 16; ++depth) {
                if (!chain.empty()) chain += " -> ";
                chain += type->name.c_str();
                type = type->parentTypeInfo.get();
            }
            return chain.empty() ? "<no type>" : chain;
        }
    }

    namespace
    {
        RE::TESNPC* NpcBaseOf(RE::TESObjectREFR* ref)
        {
            if (!ref) return nullptr;
            // TESObjectREFR::data.objectReference; read correctly for the player (00000007).
            auto base = ref->GetBaseObject();
            if (!base || !base->Is(RE::FormType::kNPC_)) return nullptr;
            return static_cast<RE::TESNPC*>(base.get());
        }

        bool DispatchStatic(const char* object, const char* function, ArgsFn args,
                            std::function<void(const Variable&)> onResult, std::function<void()> onCancel)
        {
            auto* vm = VM();
            if (!vm) {
                return false;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            callback.reset(new Callback(std::move(onResult), std::move(onCancel)));
            const RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<Variable>&)> packer =
                [args = std::move(args)](RE::BSScrapArray<Variable>& a) {
                    args(a);
                    return true;
                };
            return vm->DispatchStaticCall(object, function, packer, callback, 0);
        }
    }

    std::uint32_t PlayerRefId()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player ? player->GetFormID() : 0;
    }

    bool IsPlaceableBase(std::uint32_t baseId)
    {
        if (baseId == 0 || (baseId >> 24) == 0xFF) {
            return false;
        }
        const auto* form = RE::TESForm::LookupByID(baseId);
        return form && form->Is(RE::FormType::kNPC_);
    }

    bool RequestBaseObject(std::uint32_t actorId, std::shared_ptr<std::atomic<std::uint32_t>> result,
                           std::string& error)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) {
            error = std::format("actor {:08X} not found", actorId);
            return false;
        }
        // Form GetBaseObject() native (signature from the game's objectreference.pex).
        const bool dispatched = Dispatch(
            ScriptObjectFor(actor), "GetBaseObject", [](RE::BSScrapArray<Variable>&) {},
            [result](const Variable& v) {
                std::uint32_t id = kSpawnReturnedNone;
                if (v.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(v)) {
                    // Only an NPC_ base is usable; other forms fail to unpack as TESNPC.
                    if (auto* npc = RE::BSScript::UnpackVariable<RE::TESNPC>(v)) {
                        id = npc->GetFormID();
                    } else {
                        id = kSpawnWrongType;
                    }
                }
                result->store(id);
            },
            [result] { result->store(kSpawnCanceled); });
        if (!dispatched) {
            error = "VM refused GetBaseObject dispatch";
        }
        return dispatched;
    }

    bool RequestWeaponState(std::uint32_t actorId, std::shared_ptr<WeaponQuery> q)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) return false;
        const auto self = ScriptObjectFor(actor);
        const bool a = Dispatch(
            self, "GetEquippedWeapon", [](RE::BSScrapArray<Variable>& args) { Push(args, static_cast<std::int32_t>(0)); },
            [q](const Variable& v) {
                std::uint32_t id = 0;
                if (v.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(v)) {
                    if (auto* w = RE::BSScript::UnpackVariable<RE::TESObjectWEAP>(v)) id = w->GetFormID();
                }
                q->formId.store(id);
                q->pending.fetch_sub(1);
            },
            [q] { q->pending.fetch_sub(1); });
        const bool b = Dispatch(
            self, "IsWeaponDrawn", [](RE::BSScrapArray<Variable>&) {},
            [q](const Variable& v) {
                q->drawn.store(v.is<bool>() ? (RE::BSScript::get<bool>(v) ? 1 : 0) : -1);
                q->pending.fetch_sub(1);
            },
            [q] { q->pending.fetch_sub(1); });
        if (!a) q->pending.fetch_sub(1);
        if (!b) q->pending.fetch_sub(1);
        return a || b;
    }

    bool EquipWeapon(std::uint32_t actorId, std::uint32_t weaponFormId)
    {
        auto* actor = LookupActor(actorId);
        auto* weapon = RE::TESForm::LookupByID(weaponFormId);
        if (!actor || !weapon || !weapon->Is(RE::FormType::kWEAP)) return false;
        const auto self = ScriptObjectFor(actor);
        return Dispatch(
            self, "AddItem",
            [weapon](RE::BSScrapArray<Variable>& a) {
                Push(a, weapon);
                Push(a, static_cast<std::int32_t>(1));
                Push(a, true);  // abSilent
            },
            [self, weapon](const Variable&) {
                Dispatch(self, "EquipItem", [weapon](RE::BSScrapArray<Variable>& a) {
                    Push(a, weapon);
                    Push(a, true);  // abPreventRemoval
                    Push(a, true);  // abSilent
                });
            });
    }

    bool UnequipWeapon(std::uint32_t actorId, std::uint32_t weaponFormId)
    {
        auto* actor = LookupActor(actorId);
        auto* weapon = RE::TESForm::LookupByID(weaponFormId);
        if (!actor || !weapon || !weapon->Is(RE::FormType::kWEAP)) return false;
        return Dispatch(ScriptObjectFor(actor), "UnequipItem", [weapon](RE::BSScrapArray<Variable>& a) {
            Push(a, weapon);
            Push(a, false);  // abPreventEquip
            Push(a, true);   // abSilent
        });
    }

    bool SetWeaponDrawn(std::uint32_t actorId, bool drawn)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) return false;
        const auto self = ScriptObjectFor(actor);
        Dispatch(self, "SetAlert", [drawn](RE::BSScrapArray<Variable>& a) { Push(a, drawn); });
        return Dispatch(self, drawn ? "DrawWeapon" : "SheatheWeapon", [](RE::BSScrapArray<Variable>&) {});
    }

    bool RequestCombatTarget(std::uint32_t actorId, std::shared_ptr<std::atomic<std::uint32_t>> result)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) return false;
        return Dispatch(
            ScriptObjectFor(actor), "GetCombatTarget", [](RE::BSScrapArray<Variable>&) {},
            [result](const Variable& v) {
                std::uint32_t id = 1;
                if (v.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(v)) {
                    if (auto* t = RE::BSScript::UnpackVariable<RE::Actor>(v)) id = t->GetFormID();
                }
                result->store(id);
            },
            [result] { result->store(1); });
    }

    bool StartCombat(std::uint32_t actorId, std::uint32_t targetRefId)
    {
        auto* actor = LookupActor(actorId);
        auto* target = LookupActor(targetRefId);
        if (!actor || !target) return false;
        return Dispatch(ScriptObjectFor(actor), "StartCombat", [target](RE::BSScrapArray<Variable>& a) {
            Push(a, static_cast<RE::TESObjectREFR*>(target));
            Push(a, true);  // abPreferredTarget
        });
    }

    bool SetOpen(std::uint32_t refId, bool open)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
        if (!ref) return false;
        return Dispatch(ScriptObjectFor(ref), "SetOpen", [open](RE::BSScrapArray<Variable>& a) { Push(a, open); });
    }

    bool DisableNoWait(std::uint32_t refId)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
        if (!ref) return false;
        return Dispatch(ScriptObjectFor(ref), "DisableNoWait", [](RE::BSScrapArray<Variable>& a) { Push(a, false); });
    }

    bool SetQuestStage(std::uint32_t questId, int stage)
    {
        auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(questId);
        if (!quest) return false;
        return Dispatch(ScriptObjectFor(quest), "SetCurrentStageID", [stage](RE::BSScrapArray<Variable>& a) { Push(a, stage); });
    }

    bool SetWeatherActive(std::uint32_t weatherId)
    {
        auto* weather = RE::TESForm::LookupByID(weatherId);
        if (!weather) return false;
        return Dispatch(ScriptObjectFor(weather), "SetActive", [](RE::BSScrapArray<Variable>& a) {
            Push(a, true);   // abOverride
            Push(a, false);  // abAccelerate
        });
    }

    bool ContainerAdd(std::uint32_t refId, std::uint32_t itemId, int count)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
        auto* item = RE::TESForm::LookupByID(itemId);
        if (!ref || !item) return false;
        return Dispatch(ScriptObjectFor(ref), "AddItem", [item, count](RE::BSScrapArray<Variable>& a) {
            Push(a, item);
            Push(a, count);
            Push(a, true);  // abSilent
        });
    }

    bool ContainerRemove(std::uint32_t refId, std::uint32_t itemId, int count)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
        auto* item = RE::TESForm::LookupByID(itemId);
        if (!ref || !item) return false;
        return Dispatch(ScriptObjectFor(ref), "RemoveItem", [item, count](RE::BSScrapArray<Variable>& a) {
            Push(a, item);
            Push(a, count);
            Push(a, true);  // abSilent
            PushNone(a);    // akOtherContainer
        });
    }

    bool ContainerTakeAll(std::uint32_t refId)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!ref || !player) return false;
        return Dispatch(ScriptObjectFor(ref), "RemoveAllItems", [player](RE::BSScrapArray<Variable>& a) {
            Push(a, static_cast<RE::TESObjectREFR*>(player));
            Push(a, false);  // abKeepOwnership
            Push(a, false);  // abRemoveQuestItems
        });
    }

    bool ActivateAsPlayer(std::uint32_t refId)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!ref || !player) return false;
        return Dispatch(ScriptObjectFor(ref), "Activate", [player](RE::BSScrapArray<Variable>& a) {
            Push(a, static_cast<RE::TESObjectREFR*>(player));
            Push(a, false);  // abDefaultProcessingOnly
        });
    }

    bool StopCombat(std::uint32_t actorId)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) return false;
        return Dispatch(ScriptObjectFor(actor), "StopCombat", [](RE::BSScrapArray<Variable>&) {});
    }

    namespace
    {
        // A form's display name via its TESFullName component: RTTI cast through the game's
        // __RTDynamicCast import (ID 184372); type descriptors TESForm (ID 843049) and TESFullName
        // (ID 843072) verified against .rdata 2026-11-02. Falls back to the FormID in hex.
        std::string FullNameOr(RE::TESForm* form, bool idFallback = true)
        {
            std::string name;
            auto* full = starfield_cast<RE::TESFullName*>(form);
            if (full) {
                if (const char* n = full->GetFullName()) name = n;
            }
            static std::atomic<int> logged{ 0 };
            if (name.empty() && logged.fetch_add(1) < 4) {
                log::Info("[status] no full name for form {:08X} type {} ({})", form->GetFormID(),
                          static_cast<int>(form->GetFormType()), full ? "empty name" : "no TESFullName");
            }
            if (name.empty() && idFallback) name = std::format("{:08X}", form->GetFormID());
            return name;
        }
    }

    namespace
    {
        RE::TESForm* Wearable(std::uint32_t formId)
        {
            auto* item = RE::TESForm::LookupByID(formId);
            return item && (item->Is(RE::FormType::kARMO) || item->Is(RE::FormType::kWEAP)) ? item : nullptr;
        }
    }

    bool EquipForm(std::uint32_t actorId, std::uint32_t itemFormId)
    {
        auto* actor = LookupActor(actorId);
        auto* item = Wearable(itemFormId);
        if (!actor || !item) return false;
        // EquipItem gives the actor the item first if it has none (ID::Actor::EquipItem).
        return Dispatch(ScriptObjectFor(actor), "EquipItem", [item](RE::BSScrapArray<Variable>& a) {
            Push(a, item);
            Push(a, false);  // abPreventRemoval
            Push(a, true);   // abSilent
        });
    }

    bool UnequipForm(std::uint32_t actorId, std::uint32_t itemFormId)
    {
        auto* actor = LookupActor(actorId);
        auto* item = Wearable(itemFormId);
        if (!actor || !item) return false;
        return Dispatch(ScriptObjectFor(actor), "UnequipItem", [item](RE::BSScrapArray<Variable>& a) {
            Push(a, item);
            Push(a, false);  // abPreventEquip
            Push(a, true);   // abSilent
        });
    }

    bool KillSilent(std::uint32_t actorId)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) return false;
        return Dispatch(ScriptObjectFor(actor), "KillSilent", [](RE::BSScrapArray<Variable>& a) { a.emplace_back(); });
    }

    bool DamageHealth(std::uint32_t actorId, float amount)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) return false;
        const auto self = ScriptObjectFor(actor);
        return DispatchStatic(
            "Game", "GetHealthAV", [](RE::BSScrapArray<Variable>&) {},
            [self, amount](const Variable& av) {
                const Variable copy = av;
                Dispatch(self, "DamageValue", [copy, amount](RE::BSScrapArray<Variable>& a) {
                    a.emplace_back();
                    a.back() = copy;
                    Push(a, amount);
                });
            },
            [] {});
    }

    bool RequestStatus(std::uint32_t actorId, std::shared_ptr<StatusQuery> q)
    {
        auto* actor = LookupActor(actorId);
        if (!actor) return false;
        const auto self = ScriptObjectFor(actor);
        auto done = [q] { q->pending.fetch_sub(1); };
        // Health: Game.GetHealthAV() -> GetValuePercentage(av).
        const bool a = DispatchStatic(
            "Game", "GetHealthAV", [](RE::BSScrapArray<Variable>&) {},
            [q, self, done](const Variable& av) {
                const Variable copy = av;
                const bool sent = Dispatch(
                    self, "GetValuePercentage",
                    [copy](RE::BSScrapArray<Variable>& args) {
                        args.emplace_back();
                        args.back() = copy;
                    },
                    [q, done](const Variable& v) {
                        if (v.is<float>()) {
                            q->healthPercent.store(static_cast<int>(std::clamp(RE::BSScript::get<float>(v), 0.0f, 1.0f) * 100.0f + 0.5f));
                        }
                        done();
                    },
                    done);
                if (!sent) done();
            },
            done);
        const bool b = Dispatch(
            self, "GetLevel", [](RE::BSScrapArray<Variable>&) {},
            [q, done](const Variable& v) {
                if (v.is<std::int32_t>()) q->level.store(RE::BSScript::get<std::int32_t>(v));
                done();
            },
            done);
        const bool c = Dispatch(
            self, "IsInSpace", [](RE::BSScrapArray<Variable>&) {},
            [q, done](const Variable& v) {
                if (v.is<bool>()) q->inSpace.store(RE::BSScript::get<bool>(v) ? 1 : 0);
                done();
            },
            done);
        const bool d = Dispatch(
            self, "GetCurrentPlanet", [](RE::BSScrapArray<Variable>&) {},
            [q, done](const Variable& v) {
                // The planet form (PNDT) has no usable full name (2026-11-02: name lookup came back
                // empty, planet 0003F5A1). Its Location (planet.pex: location GetLocation()) does.
                auto* planet = v.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(v)
                                   ? RE::BSScript::UnpackVariable<RE::TESForm>(v)
                                   : nullptr;
                if (!planet) {
                    done();
                    return;
                }
                {
                    std::lock_guard lock(q->lock);
                    q->planet = FullNameOr(planet);
                }
                const bool sent = Dispatch(
                    RE::BSScript::get<RE::BSScript::Object>(v), "GetLocation", [](RE::BSScrapArray<Variable>&) {},
                    [q, done](const Variable& loc) {
                        if (loc.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(loc)) {
                            if (auto* form = RE::BSScript::UnpackVariable<RE::TESForm>(loc)) {
                                std::string name = FullNameOr(form, false);
                                if (!name.empty()) {
                                    std::lock_guard lock(q->lock);
                                    q->planet = std::move(name);
                                }
                            }
                        }
                        done();
                    },
                    done);
                if (!sent) done();
            },
            done);
        if (!a) done();
        if (!b) done();
        if (!c) done();
        if (!d) done();
        return a || b || c || d;
    }

    bool RequestTemplateActor(std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            error = "no player";
            return false;
        }
        const auto pos = player->GetPosition();
        // Game.FindRandomActor(Float afX, Float afY, Float afZ, Float afRadius), global native
        // (signature from the game's game.pex). Replaces walking TESObjectCELL::references,
        // whose layout is not verified for 1.16.244 and which crashed the game (2026-10-27).
        const bool dispatched = DispatchStatic(
            "Game", "FindRandomActor",
            [pos](RE::BSScrapArray<Variable>& a) {
                Push(a, pos.x);
                Push(a, pos.y);
                Push(a, pos.z);
                Push(a, kTemplateSearchRadius);
            },
            [result](const Variable& v) {
                std::uint32_t id = kSpawnReturnedNone;
                if (v.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(v)) {
                    if (auto* actor = RE::BSScript::UnpackVariable<RE::Actor>(v)) {
                        id = actor->GetFormID();
                    } else {
                        id = kSpawnUnpackFailed;
                    }
                }
                result->store(id);
            },
            [result] { result->store(kSpawnCanceled); });
        if (!dispatched) {
            error = "VM refused Game.FindRandomActor dispatch";
        }
        return dispatched;
    }

    bool RequestSpawn(std::uint32_t baseNpcId, std::shared_ptr<std::atomic<std::uint32_t>> result,
                      std::string& error, std::string& detail)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            error = "no player";
            return false;
        }
        // Preferred: the base of a real NPC near the player, which carries its own
        // body, face and outfit. The player's base (00000007) does not; its clone
        // rendered as a lone faint head in game (2026-10-27).
        RE::TESNPC* npc = nullptr;
        if (IsPlaceableBase(baseNpcId)) {
            npc = static_cast<RE::TESNPC*>(RE::TESForm::LookupByID(baseNpcId));
            detail = std::format("base NPC {:08X} of a nearby actor", baseNpcId);
        }
        if (!npc) {
            npc = NpcBaseOf(player);
            detail = npc ? std::format("player base NPC {:08X} (fallback)", npc->GetFormID()) : "player base NPC";
        }
        if (!npc) {
            error = std::format("no usable {}", detail);
            return false;
        }

        // Verify the form converts to a script object before handing it to the VM;
        // a failed conversion would silently become None inside the placement call.
        {
            Variable probe;
            RE::BSScript::PackVariable(probe, npc);
            if (!probe.is<RE::BSScript::Object>() || !RE::BSScript::get<RE::BSScript::Object>(probe)) {
                error = std::format("{} could not be converted to a script object", detail);
                return false;
            }
            // Record what the base form actually packs as (script type chain).
            log::Info("[avatar] base form packs as [{}] (vm type id {}, form type {}); self is [{}]",
                      TypeChain(RE::BSScript::get<RE::BSScript::Object>(probe)),
                      RE::BSScript::GetVMTypeID<RE::TESNPC>(), static_cast<int>(npc->GetFormType()),
                      TypeChain(ScriptObjectFor(static_cast<RE::Actor*>(player))));
        }

        const auto self = ScriptObjectFor(static_cast<RE::Actor*>(player));
        if (!self) {
            error = "could not get the player's script object";
            return false;
        }

        // PlaceAtMe(Form, ...) was rejected by the VM with "Type mismatch for
        // argument 1" although an ActorBase object was passed (2026-10-27), so use
        // the actor-specific native, whose first parameter is exactly actorbase:
        // PlaceActorAtMe(actorbase akActorToPlace, Int aiLevelMod, location akEncLoc,
        //                Bool abForcePersist, Bool abInitiallyDisabled, Bool abDeleteWhenAble,
        //                Float[] akOffsetValues, Bool abSnapOffsetToNavmesh)
        const bool dispatched = Dispatch(
            self, "PlaceActorAtMe",
            [npc](RE::BSScrapArray<Variable>& a) {
                Push(a, npc);
                Push(a, static_cast<std::int32_t>(0));  // aiLevelMod
                PushNone(a);     // akEncLoc
                Push(a, false);  // abForcePersist
                Push(a, false);  // abInitiallyDisabled
                Push(a, true);   // abDeleteWhenAble
                PushNone(a);     // akOffsetValues
                Push(a, true);   // abSnapOffsetToNavmesh

                // Diagnostic: the raw Papyrus type of each argument actually sent
                // (0 none, 1 object, 3 int, 5 bool; >= 22 is an object type pointer).
                std::string types;
                for (std::uint32_t i = 0; i < a.size(); ++i) {
                    const auto raw = static_cast<std::uint32_t>(a[i].GetType().GetRawType());
                    types += std::format("{}{}={}", i == 0 ? "" : " ", i, raw);
                    if (a[i].is<RE::BSScript::Object>()) {
                        types += std::format("[{}]", TypeChain(RE::BSScript::get<RE::BSScript::Object>(a[i])));
                    }
                }
                log::Info("[avatar] PlaceActorAtMe args sent: {}", types);
            },
            [result](const Variable& v) {
                std::uint32_t id = kSpawnWrongType;
                if (v.is<std::nullptr_t>()) {
                    id = kSpawnReturnedNone;
                } else if (v.is<RE::BSScript::Object>()) {
                    if (!RE::BSScript::get<RE::BSScript::Object>(v)) {
                        id = kSpawnReturnedNone;
                    } else if (auto* ref = RE::BSScript::UnpackVariable<RE::TESObjectREFR>(v)) {
                        id = ref->GetFormID();
                    } else {
                        id = kSpawnUnpackFailed;
                    }
                }
                result->store(id);
            },
            [result] { result->store(kSpawnCanceled); });
        if (!dispatched) {
            error = "VM refused PlaceActorAtMe dispatch";
        }
        return dispatched;
    }

    bool VmReady()
    {
        const auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return false;
        }
        return vm->freezeState == RE::BSScript::Internal::VirtualMachine::FreezeState::kNotFrozen && !vm->overstressed;
    }

    const char* DescribeSpawnError(std::uint32_t v) noexcept
    {
        switch (v) {
        case kSpawnCanceled: return "VM cancelled the call";
        case kSpawnReturnedNone: return "PlaceActorAtMe returned None";
        case kSpawnWrongType: return "result was not an object";
        case kSpawnUnpackFailed: return "object result had no reference behind it";
        default: return "unknown";
        }
    }

    bool Freeze(std::uint32_t refId)
    {
        auto* actor = LookupActor(refId);
        if (!actor) return false;
        return Dispatch(ScriptObjectFor(actor), "EnableAI", [](RE::BSScrapArray<Variable>& a) {
            Push(a, false);  // abEnable
            Push(a, true);   // abPauseVoice
        });
    }

    namespace
    {
        constexpr std::uint32_t kXMarkerId = 0x0000003B;

        RE::TESObjectREFR* LookupRef(std::uint32_t refId)
        {
            auto* form = RE::TESForm::LookupByID(refId);
            if (!form || !(form->Is(RE::FormType::kREFR) || form->Is(RE::FormType::kACHR))) return nullptr;
            return static_cast<RE::TESObjectREFR*>(form);
        }
    }

    bool RequestMarker(std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* marker = RE::TESForm::LookupByID(kXMarkerId);
        if (!player || !marker) {
            error = !player ? "no player" : "XMarker 0000003B not found";
            return false;
        }
        if (!marker->Is(RE::FormType::kSTAT)) {
            error = std::format("form 0000003B is type {}, not STAT", static_cast<int>(marker->GetFormType()));
            return false;
        }
        const bool ok = Dispatch(
            ScriptObjectFor(static_cast<RE::Actor*>(player)), "PlaceAtMe",
            [marker](RE::BSScrapArray<Variable>& a) {
                Push(a, marker);
                Push(a, static_cast<std::int32_t>(1));  // aiCount
                Push(a, false);                         // abForcePersist
                Push(a, false);                         // abInitiallyDisabled
                Push(a, true);                          // abDeleteWhenAble
                PushNone(a);                            // akOffsetValues
                PushNone(a);                            // akAliasToFill
                Push(a, false);                         // abSnapOffsetToNavmesh
            },
            [result](const Variable& v) {
                std::uint32_t id = kSpawnReturnedNone;
                if (v.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(v)) {
                    auto* ref = RE::BSScript::UnpackVariable<RE::TESObjectREFR>(v);
                    id = ref ? ref->GetFormID() : kSpawnUnpackFailed;
                }
                result->store(id);
            },
            [result] { result->store(kSpawnCanceled); });
        if (!ok) error = "VM refused PlaceAtMe dispatch";
        return ok;
    }

    bool MoveRef(std::uint32_t refId, const Vec3& position, const Vec3& rotationRadians)
    {
        auto* ref = LookupRef(refId);
        if (!ref) return false;
        const auto self = ScriptObjectFor(ref);
        const bool moved = Dispatch(self, "SetPosition", [position](RE::BSScrapArray<Variable>& a) {
            Push(a, position.x);
            Push(a, position.y);
            Push(a, position.z);
        });
        const float yaw = Degrees(rotationRadians.z);
        Dispatch(self, "SetAngle", [yaw](RE::BSScrapArray<Variable>& a) {
            Push(a, 0.0f);
            Push(a, 0.0f);
            Push(a, yaw);
        });
        return moved;
    }

    bool RequestPathTo(std::uint32_t actorId, std::uint32_t targetId, float normalizedSpeed, float targetRadius,
                       std::shared_ptr<std::atomic<std::uint32_t>> done)
    {
        auto* actor = LookupActor(actorId);
        auto* target = LookupRef(targetId);
        if (!actor || !target) return false;
        return Dispatch(
            ScriptObjectFor(actor), "PathToReference",
            [target, normalizedSpeed, targetRadius](RE::BSScrapArray<Variable>& a) {
                Push(a, target);
                Push(a, normalizedSpeed);     // afNormalizedSpeed (1 = run)
                Push(a, 1.0f);                // afNormalizedRotationSpeed
                Push(a, targetRadius);        // afTargetRadius
                Push(a, false);               // abHardRadius
            },
            [done](const Variable& v) {
                const auto r = v.is<std::int32_t>() ? RE::BSScript::get<std::int32_t>(v) : -1;
                done->store(static_cast<std::uint32_t>(r + 1) == 0 ? 0xFFFFFFF1u : static_cast<std::uint32_t>(r + 1));
            },
            [done] { done->store(0xFFFFFFF0u); });
    }

    bool Move(std::uint32_t refId, const Vec3& position, const Vec3& rotationRadians)
    {
        auto* actor = LookupActor(refId);
        if (!actor) return false;
        const auto self = ScriptObjectFor(actor);
        const bool moved = Dispatch(self, "SetPosition", [position](RE::BSScrapArray<Variable>& a) {
            Push(a, position.x);
            Push(a, position.y);
            Push(a, position.z);
        });
        // Only yaw is meaningful for a standing character.
        const float yaw = Degrees(rotationRadians.z);
        Dispatch(self, "SetAngle", [yaw](RE::BSScrapArray<Variable>& a) {
            Push(a, 0.0f);
            Push(a, 0.0f);
            Push(a, yaw);
        });
        return moved;
    }

    namespace
    {
        // TESObjectREFR virtual 0xAC. Is3DLoaded (native 0x2075F00) calls it as
        // `call [vtbl+0x560]` with rdx = &out, then tests *rax != null and releases
        // `out` (NiRefObject refcount at +8, DeleteThis = vfunc 1). CommonLibSF only
        // declares it as Unk_AC(), so it is called through the vtable here.
        bool Has3D(RE::TESObjectREFR* ref)
        {
            using Get3D_t = void** (*)(RE::TESObjectREFR*, void**);
            constexpr std::size_t kGet3DIndex = 0xAC;
            const auto vtbl = *reinterpret_cast<void* const* const*>(ref);
            const auto fn = reinterpret_cast<Get3D_t>(vtbl[kGet3DIndex]);
            void* out = nullptr;
            void** result = fn(ref, &out);
            const bool loaded = result && *result != nullptr;
            if (out) {
                static_cast<RE::NiRefObject*>(out)->DecRefCount();
            }
            return loaded;
        }
    }

    ProxyState Probe(std::uint32_t refId)
    {
        ProxyState s;
        auto* form = RE::TESForm::LookupByID(refId);
        if (!form) return s;
        s.exists = true;
        s.isActor = form->Is(RE::FormType::kACHR);
        s.disabled = form->IsDisabled();
        s.deleted = form->IsDeleted();
        if (!s.isActor) return s;
        auto* actor = static_cast<RE::Actor*>(form);
        if (auto base = actor->GetBaseObject()) s.baseId = base->GetFormID();
        const auto* cell = actor->parentCell;
        s.parentCellId = cell ? cell->GetFormID() : 0;
        const auto pos = actor->GetPosition();
        s.position = { pos.x, pos.y, pos.z };
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            s.sameCellAsPlayer = cell && cell == player->parentCell;
            const auto p = player->GetPosition();
            const float dx = pos.x - p.x, dy = pos.y - p.y, dz = pos.z - p.z;
            s.distanceToPlayer = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        // Only ask for 3D on a live, enabled reference.
        if (!s.deleted) s.has3D = Has3D(actor);
        return s;
    }

    std::string Describe(const ProxyState& s)
    {
        if (!s.exists) return "no form (gone)";
        return std::format("{}{}{} 3D={} cell={:08X}{} base={:08X} pos=({:.1f}, {:.1f}, {:.1f}) dist={:.1f}",
                           s.isActor ? "Actor" : "NOT an actor", s.disabled ? " DISABLED" : " enabled",
                           s.deleted ? " DELETED" : "", s.has3D ? "loaded" : "NONE", s.parentCellId,
                           s.sameCellAsPlayer ? " (player's cell)" : " (NOT player's cell)", s.baseId, s.position.x,
                           s.position.y, s.position.z, s.distanceToPlayer);
    }

    bool SetScale(std::uint32_t refId, float scale)
    {
        auto* actor = LookupActor(refId);
        if (!actor) return false;
        return Dispatch(ScriptObjectFor(actor), "SetScale", [scale](RE::BSScrapArray<Variable>& a) { Push(a, scale); });
    }

    std::shared_ptr<AnimReadout> RequestAnimVars(std::uint32_t refId, std::span<const AnimVar> vars)
    {
        auto* actor = LookupActor(refId);
        if (!actor || vars.empty()) return nullptr;
        const auto self = ScriptObjectFor(actor);
        auto out = std::make_shared<AnimReadout>(vars.size());
        constexpr std::uint32_t kPending = 0x7FC00001;  // a NaN payload no getter returns
        for (auto& b : out->bits) b.store(kPending);
        out->remaining.store(static_cast<int>(vars.size()));
        for (std::size_t i = 0; i < vars.size(); ++i) {
            const auto& var = vars[i];
            const char* fn = var.kind == AnimVarKind::kFloat ? "GetAnimationVariableFloat"
                             : var.kind == AnimVarKind::kInt ? "GetAnimationVariableInt"
                                                              : "GetAnimationVariableBool";
            const std::string name = var.name;
            const bool ok = Dispatch(
                self, fn, [name](RE::BSScrapArray<Variable>& a) { Push(a, std::string_view{ name }); },
                [out, i](const Variable& v) {
                    float f = std::numeric_limits<float>::quiet_NaN();
                    if (v.is<float>()) f = RE::BSScript::get<float>(v);
                    else if (v.is<std::int32_t>()) f = static_cast<float>(RE::BSScript::get<std::int32_t>(v));
                    else if (v.is<bool>()) f = RE::BSScript::get<bool>(v) ? 1.0f : 0.0f;
                    else out->failed.fetch_add(1);
                    out->bits[i].store(std::bit_cast<std::uint32_t>(f));
                    out->remaining.fetch_sub(1);
                },
                [out] {
                    out->failed.fetch_add(1);
                    out->remaining.fetch_sub(1);
                });
            if (!ok) {
                out->failed.fetch_add(1);
                out->remaining.fetch_sub(1);
            }
        }
        return out;
    }

    std::string FormatReadout(const AnimReadout& r, std::span<const AnimVar> vars)
    {
        std::string s;
        for (std::size_t i = 0; i < vars.size() && i < r.bits.size(); ++i) {
            const float f = std::bit_cast<float>(r.bits[i].load());
            if (!s.empty()) s += ' ';
            if (std::isnan(f)) s += std::format("{}=?", vars[i].name);
            else if (vars[i].kind == AnimVarKind::kFloat) s += std::format("{}={:.3f}", vars[i].name, f);
            else s += std::format("{}={}", vars[i].name, static_cast<int>(f));
        }
        return s;
    }

    bool SetAnimFloat(std::uint32_t refId, const char* name, float value)
    {
        auto* actor = LookupActor(refId);
        if (!actor) return false;
        const std::string n = name;
        return Dispatch(ScriptObjectFor(actor), "SetAnimationVariableFloat", [n, value](RE::BSScrapArray<Variable>& a) {
            Push(a, std::string_view{ n });
            Push(a, value);
        });
    }

    bool SetAnimBool(std::uint32_t refId, const char* name, bool value)
    {
        auto* actor = LookupActor(refId);
        if (!actor) return false;
        const std::string n = name;
        return Dispatch(ScriptObjectFor(actor), "SetAnimationVariableBool", [n, value](RE::BSScrapArray<Variable>& a) {
            Push(a, std::string_view{ n });
            Push(a, value);
        });
    }

    void Despawn(std::uint32_t refId)
    {
        auto* actor = LookupRef(refId);  // actors and plain refs (path markers)
        if (!actor) return;
        const auto self = ScriptObjectFor(actor);
        Dispatch(self, "Disable", [](RE::BSScrapArray<Variable>& a) { Push(a, false); });
        Dispatch(self, "Delete", [](RE::BSScrapArray<Variable>&) {});
    }
}
