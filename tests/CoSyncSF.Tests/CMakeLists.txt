find_package(GTest CONFIG REQUIRED)

add_executable(cosync_tests
    ConcurrencyTests.cpp
    DiscoveryTests.cpp
    EndpointTests.cpp
    GnsIntegrationTests.cpp
    InterpolationTests.cpp
    ProtocolTests.cpp
    SessionTests.cpp
    UiModelTests.cpp
    WorkerTests.cpp
)
target_link_libraries(cosync_tests PRIVATE CoSyncSF::Network CoSyncSF::UI GTest::gtest GTest::gtest_main)

include(GoogleTest)
# Integration tests open real UDP sockets on 127.0.0.1; run serially.
gtest_discover_tests(cosync_tests DISCOVERY_TIMEOUT 60 PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
