add_library(core_threadpool INTERFACE)

target_include_directories(core_threadpool INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_threadpool INTERFACE core_options core_lockfree core_logging)

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(core_threadpool INTERFACE Threads::Threads)
endif()
