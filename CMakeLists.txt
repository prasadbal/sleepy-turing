cmake_minimum_required(VERSION 3.28)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # allow FetchContent deps with old cmake_minimum_required
project(marketlib VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/compiler_options.cmake)
include(cmake/dependencies.cmake)

add_subdirectory(core)
add_subdirectory(handlers)
add_subdirectory(apps)
add_subdirectory(tests)
