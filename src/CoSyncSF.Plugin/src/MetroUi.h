#pragma once

#include "cosync/ui/UiModel.h"

#include <functional>

// CoSync multiplayer UI in a MetroFramework style: flat panels, an accent bar, uppercase tab
// headers and large tile buttons, coloured from the snapshot's Theme tokens. Dear ImGui is only
// the drawing/input layer. Every action is validated with ui::ParseIntent and emitted as a
// UiCommand; nothing here touches the engine or the network. Render thread, inside an ImGui frame.
namespace cosync::plugin::metroui
{
    void Draw(const ui::UiSnapshot& s, const std::function<void(ui::UiCommand)>& emit);
}
