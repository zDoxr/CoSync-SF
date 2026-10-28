# SFSE plugin DLL (CoSyncSF.dll).
#
# Only SFSE's plugin interface header is needed at this milestone. It is MIT
# licensed (see sfse/LICENSE: "This applies to sfse/PluginAPI.h") and is
# downloaded at a pinned commit with a hash check instead of being vendored.
set(COSYNC_SFSE_COMMIT "48535cc4306ab345252bf740c20a1c6194929b0e" CACHE STRING "ianpatt/sfse commit providing PluginAPI.h (SFSE 0.2.21)")
set(COSYNC_SFSE_PLUGINAPI_SHA256 "10b834f1490b12fcab965d9c7a49f87bac3a102ed0f25a71611d499bd506c442" CACHE STRING "Expected SHA-256 of PluginAPI.h")
set(COSYNC_SFSE_PLUGINAPI_PATH "" CACHE FILEPATH "Use a local copy of sfse/PluginAPI.h instead of downloading")

set(_sfse_include "${CMAKE_BINARY_DIR}/_deps/sfse")
set(_sfse_header "${_sfse_include}/sfse/PluginAPI.h")
if(COSYNC_SFSE_PLUGINAPI_PATH)
    configure_file("${COSYNC_SFSE_PLUGINAPI_PATH}" "${_sfse_header}" COPYONLY)
elseif(NOT EXISTS "${_sfse_header}")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/ianpatt/sfse/${COSYNC_SFSE_COMMIT}/sfse/PluginAPI.h"
        "${_sfse_header}"
        EXPECTED_HASH SHA256=${COSYNC_SFSE_PLUGINAPI_SHA256}
        TLS_VERIFY ON
        STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_sfse_header}")
        message(FATAL_ERROR "Downloading SFSE PluginAPI.h failed: ${_status}. "
            "Set COSYNC_SFSE_PLUGINAPI_PATH to a local copy or disable COSYNC_BUILD_PLUGIN.")
    endif()
endif()
file(SHA256 "${_sfse_header}" _actual)
if(NOT _actual STREQUAL COSYNC_SFSE_PLUGINAPI_SHA256)
    message(FATAL_ERROR "SFSE PluginAPI.h hash mismatch (got ${_actual})")
endif()

add_library(cosync_plugin SHARED
    src/GameBridge.cpp
    src/Plugin.cpp
    src/PluginConfig.cpp
)
set_target_properties(cosync_plugin PROPERTIES OUTPUT_NAME CoSyncSF)
target_include_directories(cosync_plugin PRIVATE src "${_sfse_include}")
target_link_libraries(cosync_plugin PRIVATE CoSyncSF::Network shell32 ole32)

# Stage a ready-to-copy game folder layout: <stage>/Data/SFSE/Plugins/
set(_stage "${CMAKE_BINARY_DIR}/stage/Data/SFSE/Plugins")
add_custom_command(TARGET cosync_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:cosync_plugin>" "${_stage}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:cosync_plugin>" "${_stage}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/CoSyncSF.ini" "${_stage}/"
    VERBATIM)
