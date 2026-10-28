# CommonLibSF (GPL-3.0-or-later with modding/linking exceptions) and its
# commonlib-shared submodule, built from pinned commits with CMake.
#
# Upstream builds with xmake; this mirrors its xmake.lua (sources, include
# dirs, forced PCH, compiler flags, system libraries) so the rest of CoSync-SF
# can stay on CMake + vcpkg. Sources are fetched at configure time, not vendored.
# Licence decision recorded in docs/RESEARCH.md §3.

include(FetchContent)

set(COSYNC_COMMONLIBSF_COMMIT "9d090c4c521f4f2c1aa00bb05923c380d47ae596" CACHE STRING "libxse/commonlibsf commit (RUNTIME_LATEST = 1.16.244)")
set(COSYNC_COMMONLIB_SHARED_COMMIT "9fbb74d628134ab4ea3a3cf5c0ed3f7eeabbd01d" CACHE STRING "libxse/commonlib-shared commit (submodule of the above)")

FetchContent_Declare(commonlibsf_src
    GIT_REPOSITORY https://github.com/libxse/commonlibsf.git
    GIT_TAG ${COSYNC_COMMONLIBSF_COMMIT}
    GIT_SUBMODULES ""          # commonlib-shared is fetched separately at its pinned commit
    GIT_SHALLOW FALSE)
FetchContent_Declare(commonlib_shared_src
    GIT_REPOSITORY https://github.com/libxse/commonlib-shared.git
    GIT_TAG ${COSYNC_COMMONLIB_SHARED_COMMIT}
    GIT_SHALLOW FALSE)
# Populate only; we define the targets ourselves (upstream has no CMakeLists).
FetchContent_GetProperties(commonlibsf_src)
if(NOT commonlibsf_src_POPULATED)
    FetchContent_Populate(commonlibsf_src)
endif()
FetchContent_GetProperties(commonlib_shared_src)
if(NOT commonlib_shared_src_POPULATED)
    FetchContent_Populate(commonlib_shared_src)
endif()

find_package(spdlog CONFIG REQUIRED)

# Flags from commonlib-shared/xmake.lua (public there, so public here).
set(_commonlib_public_options
    /EHsc /permissive- /bigobj /utf-8
    /diagnostics:caret /external:W0
    /fp:contract /fp:except- /guard:cf-
    /Zc:enumTypes /Zc:preprocessor /Zc:templateScope /Zc:__cplusplus
    /wd4200 /wd4201 /wd4324
    /we4715)

function(_cosync_commonlib_target name src_root pch)
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${src_root}/src/*.cpp")
    add_library(${name} STATIC ${_sources})
    target_compile_features(${name} PUBLIC cxx_std_23)
    target_compile_options(${name} PUBLIC ${_commonlib_public_options})
    # SYSTEM: consumers compile these headers with /external:W0, not our /W4.
    target_include_directories(${name} SYSTEM PUBLIC "${src_root}/include")
    target_precompile_headers(${name} PRIVATE "${pch}")
    set_target_properties(${name} PROPERTIES FOLDER "third_party")
endfunction()

_cosync_commonlib_target(commonlib_shared "${commonlib_shared_src_SOURCE_DIR}"
    "${commonlib_shared_src_SOURCE_DIR}/src/REX/PCH.h")
target_link_libraries(commonlib_shared PUBLIC
    spdlog::spdlog
    advapi32 bcrypt d3d11 d3dcompiler dbghelp dxgi ole32 shell32 user32 version ws2_32)

_cosync_commonlib_target(commonlibsf "${commonlibsf_src_SOURCE_DIR}"
    "${commonlibsf_src_SOURCE_DIR}/include/SFSE/Impl/PCH.h")
target_link_libraries(commonlibsf PUBLIC commonlib_shared)
add_library(CommonLibSF::CommonLibSF ALIAS commonlibsf)
