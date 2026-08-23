add_library(core_memory INTERFACE)

target_include_directories(core_memory INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_memory INTERFACE core_options)

if(JEMALLOC_FOUND)
    target_link_libraries(core_memory INTERFACE PkgConfig::JEMALLOC)
    target_compile_definitions(core_memory INTERFACE SLEEPY_USE_JEMALLOC)
endif()
