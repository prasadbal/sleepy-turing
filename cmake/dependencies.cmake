include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ── spdlog (logging + bundles {fmt}) ──────────────────────────────────────────
set(SPDLOG_NO_EXCEPTIONS  ON  CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)

# ── moodycamel ConcurrentQueue (MPMC lock-free) ───────────────────────────────
FetchContent_Declare(concurrentqueue
    GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# ── toml++ (config) ───────────────────────────────────────────────────────────
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)

# ── Boost.PFR (db: reflection over plain structs, header-only) ────────────────
# Standalone repo, not the full Boost tree -- PFR is the only Boost library
# the db layer uses. The Boost::pfr target is defined below, after
# FetchContent_MakeAvailable.
#
# Pinned to the Boost 1.91.0 release tag, deliberately not a 2.x library
# tag: 2.2.0 fails to compile under GCC 15 for a struct declared locally in
# a function ("fake_object ... declared using local type ... is used but
# never defined"), which the db examples and tests do throughout. 1.91.0 is
# also the version the db layer was developed and tested against.
#
# SOURCE_SUBDIR points at a directory that doesn't exist, so the sources are
# fetched but PFR's own CMakeLists.txt is never run. It's header-only, and
# that CMakeLists adds a test/ tree whenever BUILD_TESTING is on (which
# include(CTest) turns on for this project) whose targets link against the
# rest of Boost -- Boost::core and friends -- that isn't here.
FetchContent_Declare(pfr
    GIT_REPOSITORY https://github.com/boostorg/pfr.git
    GIT_TAG        boost-1.91.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do_not_configure
)

# ── Boost.Parser (posreport: report_expr.h's expression grammar) ─────────────
# Unlike Boost.PFR (a genuinely standalone, near-zero-dependency repo),
# Boost.Parser transitively needs hana, charconv, assert, type_index, core,
# fusion, mpl, tuple, config, container_hash, throw_exception, describe, and
# mp11 (found by actually grepping every #include <boost/...> those headers
# reach, recursively, not guessed or assumed complete on the first pass --
# container_hash's own describe dependency specifically was missed on the
# first attempt and only found from a real build failure) -- no standalone
# single-repo fetch covers that. Fetched from the boostorg/boost super-repo
# instead, with GIT_SUBMODULES restricted to just those library directories
# (plus parser itself) rather than the full, much larger submodule set the
# super-repo has -- each library's own include/boost/<name>/ subtree is
# independently correctly namespaced, so adding each one's include/ as its
# own SYSTEM include directory below resolves every #include <boost/...>
# across all of them without needing the unified boost/ symlink tree
# Boost's own b2/headers build step creates (not run here -- this only
# needs the headers, same reasoning as PFR's do_not_configure SOURCE_SUBDIR).
FetchContent_Declare(boost_parser
    GIT_REPOSITORY https://github.com/boostorg/boost.git
    GIT_TAG        boost-1.91.0
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES
        libs/parser
        libs/hana
        libs/charconv
        libs/assert
        libs/type_index
        libs/core
        libs/fusion
        libs/mpl
        libs/tuple
        libs/config
        libs/container_hash
        libs/throw_exception
        libs/describe
        libs/mp11
    SOURCE_SUBDIR  do_not_configure
)

# ── tsl::robin_map (fast hash map) ───────────────────────────────────────────
FetchContent_Declare(robin_map
    GIT_REPOSITORY https://github.com/Tessil/robin-map.git
    GIT_TAG        v1.3.0
    GIT_SHALLOW    TRUE
)

# ── Howard Hinnant date (calendar / timezone) ─────────────────────────────────
set(USE_SYSTEM_TZ_DB ON  CACHE BOOL "" FORCE)
set(BUILD_TZ_LIB     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(date
    GIT_REPOSITORY https://github.com/HowardHinnant/date.git
    GIT_TAG        v3.0.1
    GIT_SHALLOW    TRUE
)

# ── pugixml (db: QueryDescriptor SQL text stored in an external XML file) ────
# Small, standalone, genuinely near-zero-dependency (unlike Boost.Parser's
# 14-library transitive pull) -- a real single-purpose XML library, not the
# full-Boost-tree situation elsewhere in this project.
set(PUGIXML_NO_XPATH ON CACHE BOOL "" FORCE)  # not needed: lookups here are by a flat name attribute, not XPath
set(PUGIXML_NO_EXCEPTIONS OFF CACHE BOOL "" FORCE) # keep default parse-exception behavior
FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG        v1.16
    GIT_SHALLOW    TRUE
)

# ── Catch2 v3 (testing) ───────────────────────────────────────────────────────
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.6.0
    GIT_SHALLOW    TRUE
)

# ── Google Benchmark ──────────────────────────────────────────────────────────
set(BENCHMARK_ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)

# ── FlatBuffers (internal IPC serialization) ──────────────────────────────────
set(FLATBUFFERS_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL       OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATLIB ON  CACHE BOOL "" FORCE)
FetchContent_Declare(flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v24.3.25
    GIT_SHALLOW    TRUE
)

# ── CLI11 (command-line argument parsing, header-only) ────────────────────────
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    GIT_SHALLOW    TRUE
)

# ── backward-cpp (crash stack traces) ─────────────────────────────────────────
set(BACKWARD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(backward
    GIT_REPOSITORY https://github.com/bombela/backward-cpp.git
    GIT_TAG        v1.6
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(
    spdlog
    concurrentqueue
    tomlplusplus
    pfr
    boost_parser
    pugixml
    robin_map
    date
    Catch2
    benchmark
    flatbuffers
    CLI11
    backward
)

# Boost.PFR: header-only, so the target is just its include directory. SYSTEM
# keeps its (heavily template-metaprogrammed) headers out of -Wall/-Wextra/
# -Wpedantic, the same way the other fetched dependencies are treated.
add_library(pfr_headers INTERFACE)
target_include_directories(pfr_headers SYSTEM INTERFACE ${pfr_SOURCE_DIR}/include)
add_library(Boost::pfr ALIAS pfr_headers)

# Boost.Parser + its transitive dependencies (see the FetchContent_Declare
# comment above for which, and why these specific ones): each restricted
# submodule's own include/ added separately, not one shared root.
add_library(boost_parser_headers INTERFACE)
foreach(_bp_lib parser hana charconv assert type_index core fusion mpl tuple config container_hash throw_exception describe mp11)
    target_include_directories(boost_parser_headers SYSTEM INTERFACE
        ${boost_parser_SOURCE_DIR}/libs/${_bp_lib}/include)
endforeach()
add_library(Boost::parser ALIAS boost_parser_headers)

# ── jemalloc (Linux only, installed via apt) ──────────────────────────────────
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(JEMALLOC IMPORTED_TARGET jemalloc)
        if(JEMALLOC_FOUND)
            message(STATUS "jemalloc found: ${JEMALLOC_VERSION}")
        else()
            message(WARNING "jemalloc not found — run scripts/setup_linux.sh")
        endif()
    endif()
endif()
