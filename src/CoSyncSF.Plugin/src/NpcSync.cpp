#include "NpcSync.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <cstring>
#include <format>

namespace cosync::plugin::npcsync
{
    namespace
    {
        constexpr std::uint64_t kProcessListsSingletonId = 937584;  // global 0x5FD9B88 (ProcessLists*)
        // Actor list as the engine itself walks it in 0x1A9A1A0 (called by Game.FindRandomActor,
        // native 0x20095D0): u32 count at +0x60, u32* handle data at +0x68. CommonLibSF places this
        // array at +0x58, which reads an empty list on 1.16.244 (runtime 2026-11-03).
        constexpr std::size_t kCountOffset = 0x60;
        constexpr std::size_t kDataOffset = 0x68;
        constexpr std::uint32_t kMaxPlausibleActors = 4096;

        bool SafeCopy(const void* src, void* dst, std::size_t n) noexcept
        {
            __try {
                std::memcpy(dst, src, n);
                return true;
            } __except (1) {
                return false;
            }
        }
    }

    Enumeration EnumerateHighActors()
    {
        Enumeration e;
        auto* lists = *REL::Relocation<std::byte**>{ REL::ID(kProcessListsSingletonId) };
        if (!lists) {
            e.check = "ProcessLists singleton is null";
            return e;
        }
        std::uint32_t count = 0;
        std::uint32_t* data = nullptr;
        if (!SafeCopy(lists + kCountOffset, &count, sizeof(count)) || !SafeCopy(lists + kDataOffset, &data, sizeof(data))) {
            e.check = "ProcessLists actor array unreadable";
            return e;
        }
        e.arraySize = count;
        if (count > kMaxPlausibleActors || (count > 0 && !data)) {
            e.check = std::format("implausible actor array (count {}); layout NOT trusted", count);
            return e;
        }
        std::vector<std::uint32_t> handles(count);
        if (count > 0 && !SafeCopy(data, handles.data(), count * sizeof(std::uint32_t))) {
            e.check = "actor handle data unreadable; layout NOT trusted";
            return e;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (const auto raw : handles) {
            RE::BSPointerHandle<RE::Actor> handle;
            static_assert(sizeof(handle) == sizeof(raw));
            std::memcpy(&handle, &raw, sizeof(raw));
            const auto ptr = handle.get();
            auto* actor = ptr.get();
            if (!actor || !actor->Is(RE::FormType::kACHR)) {
                ++e.unresolved;
                continue;
            }
            if (actor == player) continue;
            ActorInfo info;
            info.formId = actor->GetFormID();
            if (const auto base = actor->GetBaseObject()) info.baseId = base->GetFormID();
            const auto p = actor->GetPosition();
            info.position = { p.x, p.y, p.z };
            info.yaw = actor->GetAngle().z;
            e.actors.push_back(std::move(info));
        }
        e.ok = e.unresolved == 0;
        e.check = std::format("engine actor list (+0x60/+0x68): {} handle(s), {} unresolved -> layout {}", count,
                              e.unresolved, e.ok ? "VERIFIED" : "NOT verified");
        return e;
    }
}
