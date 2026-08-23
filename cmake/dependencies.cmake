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
    robin_map
    date
    Catch2
    benchmark
    flatbuffers
    CLI11
    backward
)

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
