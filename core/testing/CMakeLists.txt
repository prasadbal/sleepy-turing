add_library(core_testing INTERFACE)

target_include_directories(core_testing INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_testing INTERFACE
    test_options
    core_perfmeasure
    Catch2::Catch2WithMain
    benchmark::benchmark
)
