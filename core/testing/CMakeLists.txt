add_library(core_testing INTERFACE)

target_include_directories(core_testing INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# core_perfmeasure transitively leaks core_options' -fno-exceptions in here
# (see reenable_exceptions in cmake/compiler_options.cmake) — must come
# after it so the override lands after the leaked flag and wins.
target_link_libraries(core_testing INTERFACE
    test_options
    core_perfmeasure
    Catch2::Catch2WithMain
    benchmark::benchmark
    reenable_exceptions
)
