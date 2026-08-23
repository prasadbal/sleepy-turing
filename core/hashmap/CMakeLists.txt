add_library(core_hashmap INTERFACE)

target_include_directories(core_hashmap INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_hashmap INTERFACE core_options tsl::robin_map)
