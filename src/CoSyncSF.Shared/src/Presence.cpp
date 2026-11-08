#include "cosync/shared/Presence.h"

namespace cosync::presence
{
    bool MayShare(const Place& a, const Place& b) noexcept
    {
        if (a.locationId == 0 || b.locationId == 0) return true;
        if (a.locationId == b.locationId) return true;
        if ((a.locationId & b.locationId & kExteriorBit) == 0) return false;
        const float dx = a.position.x - b.position.x;
        const float dy = a.position.y - b.position.y;
        return dx * dx + dy * dy < kNearbyDistance * kNearbyDistance;
    }
}
