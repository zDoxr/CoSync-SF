#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The ambient crowd follows the authority's decisions (RE 12.10). CrowdManager's random choices
// cannot be seeded, so a replica keeps the engine's own walking and sitting and takes only the
// choice of target from the authority.
namespace cosync::plugin::crowd
{
    struct Order
    {
        std::uint32_t actor = 0;   // local form ID
        std::uint32_t target = 0;  // local reference handle
        bool operator==(const Order&) const = default;
    };

    struct Counters
    {
        std::uint64_t retargets = 0;
        std::uint64_t holds = 0;
    };

    bool Install(std::string& detail);
    [[nodiscard]] bool Installed() noexcept;

    // Replaces every order. An actor without one runs on its local crowd AI. Main thread.
    void SetOrders(const std::vector<Order>& orders);

    // Stops each copy's camera deciding which crowd actors update. Main thread.
    void SetInhibitionOff(bool off);

    [[nodiscard]] Counters TakeCounters() noexcept;
}
