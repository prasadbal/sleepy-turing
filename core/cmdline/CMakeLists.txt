add_library(core_cmdline STATIC src/args.cpp)

target_include_directories(core_cmdline PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# CLI11 uses exceptions internally — do not apply core_options here
target_link_libraries(core_cmdline PUBLIC compiler_options CLI11::CLI11)
