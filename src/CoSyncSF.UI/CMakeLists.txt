# Multiplayer UI core: commands, snapshots, themes and JSON for the view.
# No engine or rendering code (see docs/UI_FRAMEWORK.md), so it is unit-tested.
add_library(cosync_ui STATIC
    src/UiModel.cpp
)
add_library(CoSyncSF::UI ALIAS cosync_ui)

target_include_directories(cosync_ui PUBLIC include)
target_compile_features(cosync_ui PUBLIC cxx_std_20)
target_link_libraries(cosync_ui PUBLIC CoSyncSF::Network PRIVATE cosync_warnings)
