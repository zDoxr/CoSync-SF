// Engine/VM code for remote-player avatars. See Avatars.h for the verified
// Papyrus signatures this relies on.

#include "Avatars.h"

#include "cosync/shared/Log.h"

#include <SFSE/SFSE.h>

#include <RE/Starfield.h>

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
            auto base = ref->GetBaseObject();
            if (!base || !base->Is(RE::FormType::kNPC_)) return nullptr;
            return static_cast<RE::TESNPC*>(base.get());
        }

        // First living actor in the player's cell that is not the player.
        RE::TESNPC* NearbyNpcBase(RE::PlayerCharacter* player, std::uint32_t& refId)
        {
            const auto* cell = player->parentCell;
            if (!cell) return nullptr;
            RE::TESNPC* found = nullptr;
            cell->ForEachReference([&](const RE::NiPointer<RE::TESObjectREFR>& ref) {
                if (ref && ref.get() != player && ref->Is(RE::FormType::kACHR)) {
                    auto* actor = static_cast<RE::Actor*>(ref.get());
                    if (!actor->IsDead()) {
                        if (auto* npc = NpcBaseOf(actor)) {
                            found = npc;
                            refId = actor->GetFormID();
                            return RE::BSContainer::ForEachResult::kStop;
                        }
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            return found;
        }
    }

    bool RequestSpawn(SpawnBase which, std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error,
                      std::string& detail)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            error = "no player";
            return false;
        }
        // Base forms are read at runtime rather than assumed.
        RE::TESNPC* npc = nullptr;
        if (which == SpawnBase::PlayerBase) {
            npc = NpcBaseOf(player);
            detail = npc ? std::format("player base NPC {:08X}", npc->GetFormID()) : "player base NPC";
        } else {
            std::uint32_t source = 0;
            npc = NearbyNpcBase(player, source);
            detail = npc ? std::format("base NPC {:08X} of nearby actor {:08X}", npc->GetFormID(), source)
                         : "nearby NPC base";
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

    void Despawn(std::uint32_t refId)
    {
        auto* actor = LookupActor(refId);
        if (!actor) return;
        const auto self = ScriptObjectFor(actor);
        Dispatch(self, "Disable", [](RE::BSScrapArray<Variable>& a) { Push(a, false); });
        Dispatch(self, "Delete", [](RE::BSScrapArray<Variable>&) {});
    }
}
