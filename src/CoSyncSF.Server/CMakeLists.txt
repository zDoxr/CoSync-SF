add_executable(cosync_server main.cpp)
set_target_properties(cosync_server PROPERTIES OUTPUT_NAME cosync-server)
target_link_libraries(cosync_server PRIVATE CoSyncSF::Network)
