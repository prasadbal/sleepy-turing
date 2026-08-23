add_library(core_perfmeasure STATIC src/tsc.cpp)

target_include_directories(core_perfmeasure PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_perfmeasure PUBLIC core_options)
