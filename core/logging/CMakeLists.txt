add_library(core_logging STATIC
    src/logger.cpp
    src/drain.cpp
)

target_include_directories(core_logging PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# spdlog uses exceptions internally — do not apply core_options here
target_link_libraries(core_logging PUBLIC
    compiler_options
    core_config
    core_perfmeasure
    spdlog::spdlog
    core_lockfree
)
