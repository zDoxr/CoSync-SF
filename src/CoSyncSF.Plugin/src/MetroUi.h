#pragma once

#include "cosync/ui/UiModel.h"

#include <functional>

// CoSync in-game UI: a modern, minimal dark-glass design (Bahnschrift headings, Segoe UI body,
// Consolas codes, Segoe Fluent icons), coloured from the snapshot's Theme accents. Dear ImGui is
// only the drawing/input layer. Every action is validated with ui::ParseIntent and emitted as a
// UiCommand; nothing here touches the engine or the network. Render thread.
namespace cosync::plugin::metroui
{
    // After ImGui::CreateContext, before the renderer backend builds its font texture.
    void LoadFonts();
    // Inside an ImGui frame. Returns true when the user asked to close the UI.
    bool Draw(const ui::UiSnapshot& s, const std::function<void(ui::UiCommand)>& emit);
}
