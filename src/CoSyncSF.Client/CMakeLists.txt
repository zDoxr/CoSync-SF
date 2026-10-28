# Headless protocol test client (no game required).
add_executable(cosync_testclient main.cpp)
set_target_properties(cosync_testclient PROPERTIES OUTPUT_NAME cosync-testclient)
target_link_libraries(cosync_testclient PRIVATE CoSyncSF::Network)
