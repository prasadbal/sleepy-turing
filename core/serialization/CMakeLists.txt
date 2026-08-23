add_library(core_serialization INTERFACE)

target_include_directories(core_serialization INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_serialization INTERFACE core_options flatbuffers)
