// Engine/VM code for remote-player avatars. See Avatars.h for the verified
// Papyrus signatures this relies on.

#include "Avatars.h"

#include "cosync/shared/Log.h"

#include <SFSE/SFSE.h>

#include <RE/Starfield.h>

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
            explicit Callback(std::function<void(const Variable&)> fn) : _fn(std::move(fn)) {}
            void CallQueued() override {}
            void CallCanceled() override
            {
                if (_fn) _fn(Variable{});
            }
            void StartMultiDispatch() override {}
            void EndMultiDispatch() override {}
            void operator()(Variable result) override
            {
                if (_fn) _fn(result);
            }

        private:
            std::function<void(const Variable&)> _fn;
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
                      std::function<void(const Variable&)> onResult = {})
        {
            auto* vm = VM();
            if (!vm || !self) {
                return false;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            if (onResult) {
                callback.reset(new Callback(std::move(onResult)));
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
    }

    bool RequestSpawn(std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            error = "no player";
            return false;
        }
        // Use the player's own base NPC, read at runtime rather than assumed.
        auto base = player->GetBaseObject();
        if (!base || !base->Is(RE::FormType::kNPC_)) {
            error = "player base object is not an NPC";
            return false;
        }
        auto* npc = static_cast<RE::TESNPC*>(base.get());

        const auto self = ScriptObjectFor(static_cast<RE::Actor*>(player));
        if (!self) {
            error = "could not get the player's script object";
            return false;
        }

        // PlaceAtMe(Form akFormToPlace, Int aiCount, Bool abForcePersist, Bool abInitiallyDisabled,
        //           Bool abDeleteWhenAble, Float[] akOffsetValues, alias akAliasToFill, Bool abSnapOffsetToNavmesh)
        const bool dispatched = Dispatch(
            self, "PlaceAtMe",
            [npc](RE::BSScrapArray<Variable>& a) {
                Push(a, npc);
                Push(a, static_cast<std::int32_t>(1));
                Push(a, false);  // abForcePersist
                Push(a, false);  // abInitiallyDisabled
                Push(a, true);   // abDeleteWhenAble
                PushNone(a);     // akOffsetValues
                PushNone(a);     // akAliasToFill
                Push(a, true);   // abSnapOffsetToNavmesh
            },
            [result](const Variable& v) {
                std::uint32_t id = kSpawnFailed;
                if (v.is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(v)) {
                    if (auto* ref = RE::BSScript::UnpackVariable<RE::TESObjectREFR>(v)) {
                        id = ref->GetFormID();
                    }
                }
                result->store(id);
            });
        if (!dispatched) {
            error = "VM refused PlaceAtMe dispatch";
        }
        return dispatched;
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
