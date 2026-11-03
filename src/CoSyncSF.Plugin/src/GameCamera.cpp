#include "GameCamera.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>

#include <chrono>
#include <cmath>
#include <cstring>

namespace cosync::plugin::gamecamera
{
    namespace
    {
        constexpr std::uint64_t kPlayerCameraSingletonId = 937788;
        constexpr std::uint64_t kPlayerCameraVtableId = 459725;
        constexpr std::uint64_t kNiCameraVtableId = 498033;
        constexpr std::size_t kWorldRotate = 0x80;     // 3 rows x 4 floats
        constexpr std::size_t kWorldTranslate = 0xB0;  // NiPoint3
        constexpr std::size_t kWorldToClip = 0x180;    // 4x4 row-major

        bool SafeCopy(const void* src, void* dst, std::size_t n) noexcept
        {
            __try {
                std::memcpy(dst, src, n);
                return true;
            } __except (1) {
                return false;
            }
        }

        bool Plausible(std::uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFF && (p & 7) == 0; }

        std::uintptr_t ReadPtr(std::uintptr_t at)
        {
            std::uintptr_t v = 0;
            return Plausible(at) && SafeCopy(reinterpret_cast<const void*>(at), &v, sizeof(v)) ? v : 0;
        }

        bool ReadView(std::uintptr_t cam, View& v)
        {
            float t[3]{};
            if (!SafeCopy(reinterpret_cast<const void*>(cam + kWorldToClip), v.m, sizeof(v.m)) ||
                !SafeCopy(reinterpret_cast<const void*>(cam + kWorldTranslate), t, sizeof(t))) {
                return false;
            }
            v.position = { t[0], t[1], t[2] };
            for (float f : v.m) {
                if (!std::isfinite(f)) return false;
            }
            // Row 3 is the camera's view axis (unit) and passes through the camera position.
            const float len = std::sqrt(v.m[12] * v.m[12] + v.m[13] * v.m[13] + v.m[14] * v.m[14]);
            const float w0 = v.m[12] * t[0] + v.m[13] * t[1] + v.m[14] * t[2] + v.m[15];
            return std::fabs(len - 1.0f) < 0.02f && std::fabs(w0) < 0.05f;
        }

        struct State
        {
            std::uintptr_t camera = 0;
            std::chrono::steady_clock::time_point nextSearch{};
            int searches = 0;
            bool loggedFail = false;
        } g;

        // PlayerCamera -> (pointer | pointer -> pointer | pointer -> array) -> NiCamera, the one
        // that validates and is nearest the player.
        std::uintptr_t Find(std::uintptr_t& foundAt, std::size_t (&path)[3], int& depth)
        {
            const auto camVt = REL::ID(kNiCameraVtableId).address();
            const auto pcVt = REL::ID(kPlayerCameraVtableId).address();
            const auto pc = ReadPtr(REL::ID(kPlayerCameraSingletonId).address());
            if (!pc || ReadPtr(pc) != pcVt) return 0;
            const auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return 0;
            const auto pp = player->GetPosition();
            std::uintptr_t best = 0;
            float bestDist = 50.0f;
            auto consider = [&](std::uintptr_t obj, std::size_t a, std::size_t b, std::size_t c, int d) {
                if (ReadPtr(obj) != camVt) return;
                View v;
                if (!ReadView(obj, v)) return;
                const float dx = v.position.x - pp.x, dy = v.position.y - pp.y, dz = v.position.z - pp.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = obj;
                    path[0] = a, path[1] = b, path[2] = c;
                    depth = d;
                }
            };
            for (std::size_t a = 0; a < 0x300; a += 8) {
                const auto p1 = ReadPtr(pc + a);
                if (!p1) continue;
                consider(p1, a, 0, 0, 1);
                for (std::size_t b = 0; b < 0x200; b += 8) {
                    const auto p2 = ReadPtr(p1 + b);
                    if (!p2) continue;
                    consider(p2, a, b, 0, 2);
                    for (std::size_t c = 0; c < 0x80; c += 8) {
                        const auto p3 = ReadPtr(p2 + c);
                        if (p3) consider(p3, a, b, c, 3);
                    }
                }
            }
            foundAt = pc;
            return best;
        }
    }

    bool Read(View& out)
    {
        if (g.camera && ReadPtr(g.camera) == REL::ID(kNiCameraVtableId).address() && ReadView(g.camera, out)) {
            return true;
        }
        g.camera = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now < g.nextSearch) return false;
        g.nextSearch = now + std::chrono::seconds(2);
        std::uintptr_t pc = 0;
        std::size_t path[3]{};
        int depth = 0;
        g.camera = Find(pc, path, depth);
        ++g.searches;
        if (g.camera) {
            log::Info("[camera] NiCamera found: PlayerCamera {:X} path +{:X}{}{} (depth {}), validated", pc, path[0],
                      depth >= 2 ? std::format(" -> +{:X}", path[1]) : std::string(),
                      depth >= 3 ? std::format(" -> +{:X}", path[2]) : std::string(), depth);
            g.loggedFail = false;
            return ReadView(g.camera, out);
        }
        if (!g.loggedFail) {
            log::Warn("[camera] no validated NiCamera reachable from PlayerCamera; overhead markers hidden");
            g.loggedFail = true;
        }
        return false;
    }

    bool Project(const View& v, const Vec3& p, float& x, float& y)
    {
        const auto& m = v.m;
        const float w = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
        if (w < 0.1f) return false;
        x = (m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3]) / w;
        y = (m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7]) / w;
        return std::isfinite(x) && std::isfinite(y);
    }
}
