# SFSE plugin DLL (CoSyncSF.dll), built on CommonLibSF (see cmake/CommonLibSF.cmake).
add_library(cosync_plugin SHARED
    src/GameBridge.cpp
    src/GameState.cpp
    src/Plugin.cpp
    src/PluginConfig.cpp
)
set_target_properties(cosync_plugin PROPERTIES OUTPUT_NAME CoSyncSF)
target_compile_features(cosync_plugin PRIVATE cxx_std_23)
target_include_directories(cosync_plugin PRIVATE src)
target_link_libraries(cosync_plugin PRIVATE CoSyncSF::Network CommonLibSF::CommonLibSF shell32 ole32)

# Stage a ready-to-copy game folder layout: <stage>/Data/SFSE/Plugins/
set(_stage "${CMAKE_BINARY_DIR}/stage/Data/SFSE/Plugins")
add_custom_command(TARGET cosync_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:cosync_plugin>" "${_stage}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:cosync_plugin>" "${_stage}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/CoSyncSF.ini" "${_stage}/"
    VERBATIM)
