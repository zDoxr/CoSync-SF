#pragma once

#include "cosync/ui/UiModel.h"

#include <cstdint>
#include <string>
#include <vector>

// In-game UI overlay: renders the CoSync Metro-style UI (MetroUi) on the game's D3D12 swapchain.
// Hooks (MinHook trampolines on the real DXGI/D3D12 functions, so they also fire behind the
// game's Streamline swapchain wrapper): IDXGISwapChain::Present / ResizeBuffers and
// ID3D12CommandQueue::ExecuteCommandLists (to capture the game's direct queue). Input: the game
// window's WndProc is subclassed; F7 toggles the UI, and while it is open mouse/keyboard input
// goes to the UI only.
//
// Threads: Present runs on the game's render thread. The main thread publishes snapshots and
// drains the commands the UI produced; both are exchanged under one mutex.
namespace cosync::plugin::overlay
{
    // HUD for group members (party frames, overhead markers), published by the main thread.
    struct HudMember
    {
        std::string name;
        int level = -1;
        int healthPercent = -1;
        std::string place;          // planet name
        std::uint8_t placeKind = 0;  // proto::PlaceKind (2 = in space / orbit)
        float distance = -1.0f;     // meters; -1 = not in the same area
        bool marker = false;        // overhead marker on screen at (x, y) pixels
        float x = 0.0f, y = 0.0f;   // NDC: -1..1, y up
        float alpha = 1.0f;         // fades with distance
    };
    struct HudState
    {
        bool gameMenuOpen = false;  // the game's main/pause menu: show the Multiplayer launcher
        std::vector<HudMember> members;
    };
    void PublishHud(HudState hud);

    // Installs the hooks (call once at kPostDataLoad, main thread, after the game has a device).
    bool Install(std::string& detail);
    // Main thread: the snapshot the UI renders next.
    void Publish(const ui::UiSnapshot& snapshot);
    // Main thread: commands the UI produced since the last call, in order.
    void DrainCommands(std::vector<ui::UiCommand>& out);
    [[nodiscard]] bool IsOpen() noexcept;
}
