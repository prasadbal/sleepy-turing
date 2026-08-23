add_library(compiler_options INTERFACE)

target_compile_options(compiler_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3 -march=native>
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -march=native>
    >
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4 /permissive-
        $<$<CONFIG:Release>:/O2>
    >
)

# Core libs: no exceptions, no RTTI — std::expected handles errors
add_library(core_options INTERFACE)
target_link_libraries(core_options INTERFACE compiler_options)
target_compile_options(core_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions -fno-rtti>
    $<$<CXX_COMPILER_ID:MSVC>:/EHs- /GR->
)

# Test targets: exceptions enabled for Catch2
add_library(test_options INTERFACE)
target_link_libraries(test_options INTERFACE compiler_options)

# Some otherwise exception-free targets (core_app, test executables) end up
# transitively depending on header-only core_* libraries that PUBLICLY link
# core_options for their own internal use — e.g. core_lockfree's SpscQueue is
# embedded directly in logging's public headers, and core_perfmeasure is
# pulled into core_testing. That leaks -fno-exceptions/-fno-rtti into
# consumers that need exceptions for a third-party dependency (CLI11,
# backward-cpp, spdlog, Catch2) even though the consumer's own CMakeLists
# never asked for core_options.
#
# A target's own compile options land BEFORE options inherited via
# target_link_libraries(), so calling target_compile_options() directly on
# the affected target loses to the inherited -fno-exceptions (last flag on
# the command line wins). Instead, link this INTERFACE library LAST in the
# affected target's target_link_libraries() call — its
# INTERFACE_COMPILE_OPTIONS then land after the leaked flags and win.
# RTTI is deliberately left disabled: exception catch-matching doesn't
# require -frtti (that only gates explicit typeid/dynamic_cast), so there's
# no need to re-enable it just to support throwing/catching.
add_library(reenable_exceptions INTERFACE)
target_compile_options(reenable_exceptions INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fexceptions>
    $<$<CXX_COMPILER_ID:MSVC>:/EHs>
)
