# Standalone build for the binding/ idea -- deliberately NOT wired into the
# top-level sleepy-turing CMakeLists.txt / add_subdirectory() tree, since
# boost::pfr isn't (yet) on the project's approved-libraries list. Build it
# directly:
#
#   cmake -S ideas/binding -B ideas/binding/build -DBOOST_ROOT=/path/to/boost
#   cmake --build ideas/binding/build
#   ./ideas/binding/build/binding_demo
#
# Only boost/pfr.hpp is needed (header-only), so this looks for an existing
# Boost install first -- pass -DBOOST_ROOT=<install> to point at one, or set
# the BOOST_ROOT env var -- and only falls back to fetching a standalone pfr
# checkout from GitHub if nothing local is found.

cmake_minimum_required(VERSION 3.20)
project(binding_idea CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(BINDING_BOOST_INCLUDE_DIR boost/pfr.hpp
    HINTS ${BOOST_ROOT} ENV BOOST_ROOT
    PATH_SUFFIXES include
)

add_library(binding INTERFACE)
target_include_directories(binding INTERFACE include)

if(BINDING_BOOST_INCLUDE_DIR)
    message(STATUS "binding: using existing Boost.PFR from ${BINDING_BOOST_INCLUDE_DIR}")
    target_include_directories(binding INTERFACE ${BINDING_BOOST_INCLUDE_DIR})
else()
    message(STATUS "binding: no local boost/pfr.hpp found, fetching standalone pfr from GitHub")
    include(FetchContent)
    FetchContent_Declare(
        pfr
        GIT_REPOSITORY https://github.com/boostorg/pfr.git
        GIT_TAG        2.2.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(pfr)
    target_link_libraries(binding INTERFACE Boost::pfr)
endif()

add_executable(binding_demo examples/main.cpp)
target_link_libraries(binding_demo PRIVATE binding)

add_executable(collection_demo examples/collection_demo.cpp)
target_link_libraries(collection_demo PRIVATE binding)

# A timing comparison, not a correctness demo -- always optimized regardless
# of the overall build type, since an unoptimized comparison of an O(1)
# hash lookup against an O(N) linear scan mostly measures debug-build
# overhead, not the algorithmic difference this is supposed to show.
add_executable(lookup_benchmark examples/lookup_benchmark.cpp)
target_link_libraries(lookup_benchmark PRIVATE binding)
target_compile_options(lookup_benchmark PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/O2>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2>
)

# config_demo additionally needs boost::property_tree, which (unlike pfr)
# isn't available through the standalone-pfr FetchContent fallback above --
# it needs an actual local Boost install. Only build it when one was found.
if(BINDING_BOOST_INCLUDE_DIR)
    add_executable(config_demo examples/config_demo.cpp)
    target_link_libraries(config_demo PRIVATE binding)

    # The only compiled pieces of this otherwise header-only library.
    # field_tree_parser.cpp is the sole translation unit that names a
    # parser library (boost::property_tree for .xml/.json, and toml++ for
    # .toml when it's available -- detected with __has_include, so this
    # builds either way); configuration.cpp is the only one that names
    # which parser is in use at all. No header a caller includes mentions
    # either, so swapping the backend is one alias in configuration.cpp
    # plus its parser's own .cpp -- callers don't recompile for it.
    add_library(binding_config STATIC
        src/configuration.cpp
        src/field_tree_parser.cpp
    )
    target_link_libraries(binding_config PUBLIC binding)
else()
    message(STATUS "binding: no local Boost install found, skipping config_demo and binding_config (both need boost::property_tree)")
endif()
