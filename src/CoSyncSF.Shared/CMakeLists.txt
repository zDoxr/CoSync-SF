# Protocol definitions, serialization and shared types. No platform or
# transport dependencies so it can be used by plugin, server and tests alike.
add_library(cosync_shared STATIC
    src/GameRuntime.cpp
    src/Log.cpp
    src/Protocol.cpp
)
add_library(CoSyncSF::Shared ALIAS cosync_shared)

target_include_directories(cosync_shared PUBLIC include)
target_link_libraries(cosync_shared PUBLIC cosync_warnings)
