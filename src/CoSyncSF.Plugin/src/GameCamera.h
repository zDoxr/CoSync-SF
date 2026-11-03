#pragma once

#include "cosync/shared/Types.h"

// The game's main view camera, read for world-to-screen projection (overhead markers).
//
// Evidence (Starfield 1.16.244, 2026-11-02):
//   NiCamera vtable ID 498033 (RVA 0x4EF0D60). Ctor 0x2BE2BE0 sets the frustum at +0x1C0
//   (l,r,t,b,near,far = -0.916,0.916,0.515,-0.515,0.1,20000; ortho flag +0x1D8).
//   UpdateWorldData (vtable slot 79, 0x2BE4750) calls 0x2BE50F0(&worldTranslate(+0xB0),
//   worldRotate(+0x80), frustum(+0x1C0), ortho, out = +0x180). 0x2BE50F0 writes a row-major 4x4
//   world-to-clip matrix; perspective row 3 = (R row 0, -dot(R row 0, translate)), i.e. w is the
//   depth along the camera's view axis. NDC x, y = row0.p / w, row1.p / w (y up).
//   PlayerCamera singleton ID 937788, vtable ID 459725.
//
// The NiCamera instance is found by scanning outward from PlayerCamera for an object with the
// NiCamera vtable (SEH-guarded reads). It is used only while it passes a per-frame self-check:
// finite matrix, unit row 3, and w(camera position) ~ 0. Otherwise nothing is projected.
namespace cosync::plugin::gamecamera
{
    struct View
    {
        float m[16]{};  // row-major world-to-clip
        Vec3 position{};
    };

    // Main thread. False if the camera is not found or fails the self-check.
    bool Read(View& out);

    // World point to NDC (x right, y up, both -1..1 on screen). False if behind the camera.
    bool Project(const View& v, const Vec3& p, float& x, float& y);
}
