add_library(core_time INTERFACE)

target_include_directories(core_time INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_time INTERFACE core_options core_perfmeasure)
