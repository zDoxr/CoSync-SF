#pragma once

#include "cosync/ui/UiModel.h"

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
    // Installs the hooks (call once at kPostDataLoad, main thread, after the game has a device).
    bool Install(std::string& detail);
    // Main thread: the snapshot the UI renders next.
    void Publish(const ui::UiSnapshot& snapshot);
    // Main thread: commands the UI produced since the last call, in order.
    void DrainCommands(std::vector<ui::UiCommand>& out);
    [[nodiscard]] bool IsOpen() noexcept;
}
