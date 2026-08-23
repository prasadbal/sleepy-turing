add_library(core_lockfree INTERFACE)

target_include_directories(core_lockfree INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_lockfree INTERFACE core_options)
