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
