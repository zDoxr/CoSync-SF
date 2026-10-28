# Transport abstraction, GNS backend, in-memory loopback backend, session
# protocol state machines and LAN discovery. No Starfield engine code here.
add_library(cosync_network STATIC
    src/Endpoint.cpp
    src/GnsTransport.cpp
    src/LanDiscovery.cpp
    src/LoopbackTransport.cpp
    src/NetworkWorker.cpp
    src/SessionClient.cpp
    src/SessionServer.cpp
    src/Util.cpp
)
add_library(CoSyncSF::Network ALIAS cosync_network)

target_include_directories(cosync_network PUBLIC include)
target_link_libraries(cosync_network
    PUBLIC
        CoSyncSF::Shared
    PRIVATE
        GameNetworkingSockets::static
        ws2_32
        bcrypt
)
