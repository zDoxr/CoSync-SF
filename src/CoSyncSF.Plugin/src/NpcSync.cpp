#include "NpcSync.h"

#include "Avatars.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <algorithm>
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

    std::vector<ActorInfo> EnumerateCrowdActors()
    {
        std::vector<ActorInfo> out;
        constexpr std::uint64_t kCrowdManagerId = 937582;  // global 0x5FD9B70
        constexpr std::size_t kLockOffset = 0x50;          // read-locked by 0x12824F0
        constexpr std::size_t kCountOffset = 0x68;
        constexpr std::size_t kDataOffset = 0x70;
        constexpr std::size_t kEntrySize = 0x80;           // +0x00: the actor's handle (0x1272660)
        auto* crowd = *REL::Relocation<std::byte**>{ REL::ID(kCrowdManagerId) };
        if (!crowd) return out;
        struct Seen
        {
            std::uint32_t actor;
            std::uint32_t target;
        };
        std::vector<Seen> handles;
        {
            auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(crowd + kLockOffset);
            lock->LockRead();
            const auto count = *reinterpret_cast<const std::uint32_t*>(crowd + kCountOffset);
            const auto* data = *reinterpret_cast<const std::byte* const*>(crowd + kDataOffset);
            if (count <= kMaxPlausibleActors && data) {
                handles.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto* entry = reinterpret_cast<const std::uint32_t*>(data + i * kEntrySize);
                    handles.push_back({ entry[0], entry[1] });  // +0x04: its target, stored by 0x1271B20
                }
            }
            lock->UnlockRead();
        }
        for (const auto& seen : handles) {
            RE::BSPointerHandle<RE::Actor> handle;
            std::memcpy(&handle, &seen.actor, sizeof(seen.actor));
            const auto ptr = handle.get();
            auto* actor = ptr.get();
            if (!actor || !actor->Is(RE::FormType::kACHR)) continue;
            ActorInfo info;
            info.formId = actor->GetFormID();
            if (const auto base = actor->GetBaseObject()) info.baseId = base->GetFormID();
            const auto p = actor->GetPosition();
            info.position = { p.x, p.y, p.z };
            info.yaw = actor->GetAngle().z;
            RE::BSPointerHandle<RE::TESObjectREFR> targetHandle;
            std::memcpy(&targetHandle, &seen.target, sizeof(seen.target));
            const auto targetPtr = targetHandle.get();
            if (const auto* target = targetPtr.get()) {
                const auto at = target->GetPosition();
                info.hasCrowdTarget = true;
                info.crowdTarget = { at.x, at.y, at.z };
            }
            out.push_back(std::move(info));
        }
        return out;
    }

    std::vector<npc::CrowdTarget> EnumerateCrowdTargets()
    {
        std::vector<npc::CrowdTarget> out;
        constexpr std::uint64_t kCrowdManagerId = 937582;
        constexpr std::size_t kLockOffset = 0x50;
        constexpr std::size_t kCountOffset = 0x78;  // 0x1278150 walks this array by handle
        constexpr std::size_t kDataOffset = 0x80;
        constexpr std::size_t kEntrySize = 80;
        constexpr std::size_t kHandleOffset = 56;
        constexpr std::uint32_t kMaxPlausibleTargets = 32768;
        auto* crowd = *REL::Relocation<std::byte**>{ REL::ID(kCrowdManagerId) };
        if (!crowd) return out;
        std::vector<std::uint32_t> handles;
        {
            auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(crowd + kLockOffset);
            lock->LockRead();
            const auto count = *reinterpret_cast<const std::uint32_t*>(crowd + kCountOffset);
            const auto* data = *reinterpret_cast<const std::byte* const*>(crowd + kDataOffset);
            if (count <= kMaxPlausibleTargets && data) {
                handles.resize(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (!SafeCopy(data + i * kEntrySize + kHandleOffset, &handles[i], sizeof(std::uint32_t))) {
                        handles.resize(i);
                        break;
                    }
                }
            }
            lock->UnlockRead();
        }
        out.reserve(handles.size());
        for (const auto raw : handles) {
            RE::BSPointerHandle<RE::TESObjectREFR> handle;
            std::memcpy(&handle, &raw, sizeof(raw));
            const auto ptr = handle.get();
            if (const auto* target = ptr.get()) {
                const auto p = target->GetPosition();
                out.push_back({ raw, { p.x, p.y, p.z } });
            }
        }
        return out;
    }

    std::vector<ActorInfo> EnumerateOtherLoadedActors()
    {
        std::vector<ActorInfo> out;
        auto* lists = *REL::Relocation<std::byte**>{ REL::ID(kProcessListsSingletonId) };
        if (!lists) return out;
        std::uint32_t mainCount = 0;
        std::uint32_t* mainData = nullptr;
        std::vector<std::uint32_t> seen;
        if (SafeCopy(lists + kCountOffset, &mainCount, sizeof(mainCount)) && SafeCopy(lists + kDataOffset, &mainData, sizeof(mainData)) &&
            mainCount <= kMaxPlausibleActors && mainData) {
            seen.resize(mainCount);
            if (!SafeCopy(mainData, seen.data(), mainCount * sizeof(std::uint32_t))) seen.clear();
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (const std::size_t off : { std::size_t{ 0x70 }, std::size_t{ 0x90 }, std::size_t{ 0x190 } }) {
            std::uint32_t count = 0;
            std::uint32_t* data = nullptr;
            if (!SafeCopy(lists + off, &count, sizeof(count)) || !SafeCopy(lists + off + 8, &data, sizeof(data))) continue;
            if (count == 0 || count > kMaxPlausibleActors || !data) continue;
            std::vector<std::uint32_t> handles(count);
            if (!SafeCopy(data, handles.data(), count * sizeof(std::uint32_t))) continue;
            for (const auto raw : handles) {
                if (std::ranges::find(seen, raw) != seen.end()) continue;
                seen.push_back(raw);
                RE::BSPointerHandle<RE::Actor> handle;
                std::memcpy(&handle, &raw, sizeof(raw));
                const auto ptr = handle.get();
                auto* actor = ptr.get();
                if (!actor || actor == player || !actor->Is(RE::FormType::kACHR)) continue;
                const auto form = actor->GetFormID();
                if (!avatars::Probe(form).has3D) continue;
                ActorInfo info;
                info.formId = form;
                if (const auto base = actor->GetBaseObject()) info.baseId = base->GetFormID();
                const auto p = actor->GetPosition();
                info.position = { p.x, p.y, p.z };
                info.yaw = actor->GetAngle().z;
                out.push_back(std::move(info));
            }
        }
        return out;
    }

    std::string ScanActorLists()
    {
        auto* lists = *REL::Relocation<std::byte**>{ REL::ID(kProcessListsSingletonId) };
        if (!lists) return "ProcessLists singleton is null";
        std::string out;
        for (std::size_t off = 0; off + 0x10 <= 0x200; off += 8) {
            std::uint32_t count = 0;
            std::uint32_t* data = nullptr;
            if (!SafeCopy(lists + off, &count, sizeof(count)) || !SafeCopy(lists + off + 8, &data, sizeof(data))) continue;
            if (count == 0 || count > kMaxPlausibleActors || !data) continue;
            std::vector<std::uint32_t> handles(count);
            if (!SafeCopy(data, handles.data(), count * sizeof(std::uint32_t))) continue;
            std::uint32_t actors = 0;
            for (const auto raw : handles) {
                RE::BSPointerHandle<RE::Actor> handle;
                std::memcpy(&handle, &raw, sizeof(raw));
                const auto ptr = handle.get();
                if (ptr.get() && ptr.get()->Is(RE::FormType::kACHR)) ++actors;
            }
            if (actors > 0) out += std::format(" +{:#x}: {} of {}", off, actors, count);
        }
        return out.empty() ? " none" : out;
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
