#pragma once

#include <string>

// Keeps the world running while a menu is open in a session (RE 12.8).
//
// Main::Update (0x1888410, ID 99406) sets Main+0x448, "the game is paused", every frame from
// UI+0x4B4 (the menu pause counter), an open MainMenu, or a global. Starfield's other systems read
// that flag. The patch ignores the counter at this site, unless a loading screen is open or no
// session is active, in which case the game pauses as usual.
namespace cosync::plugin::menupause
{
    // Checks the exact instruction bytes first and refuses on any difference. Needs the trampoline.
    bool Install(std::string& detail);
    // Menus stop pausing only while this is true. Thread-safe.
    void SetActive(bool active) noexcept;
}
