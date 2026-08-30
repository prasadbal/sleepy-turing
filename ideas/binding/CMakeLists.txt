# Standalone build for the binding/ idea -- deliberately NOT wired into the
# top-level sleepy-turing CMakeLists.txt / add_subdirectory() tree, since
# boost::pfr isn't (yet) on the project's approved-libraries list. Build it
# directly:
#
#   cmake -S ideas/binding -B ideas/binding/build
#   cmake --build ideas/binding/build
#   ./ideas/binding/build/binding_demo

cmake_minimum_required(VERSION 3.20)
project(binding_idea CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    pfr
    GIT_REPOSITORY https://github.com/boostorg/pfr.git
    GIT_TAG        2.2.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pfr)

add_library(binding INTERFACE)
target_include_directories(binding INTERFACE include)
target_link_libraries(binding INTERFACE Boost::pfr)

add_executable(binding_demo examples/main.cpp)
target_link_libraries(binding_demo PRIVATE binding)
