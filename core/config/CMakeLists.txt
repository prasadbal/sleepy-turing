add_library(core_config STATIC src/config.cpp)

target_include_directories(core_config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# toml++ uses exceptions internally — do not apply core_options (-fno-exceptions) here
target_link_libraries(core_config PUBLIC compiler_options tomlplusplus::tomlplusplus)
