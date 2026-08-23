add_library(handlers_onload STATIC src/onload_receiver.cpp)

target_include_directories(handlers_onload PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(handlers_onload PUBLIC core_options core_logging)

# Onload's extensions API (onload_set_stackname, onload_thread_set_spin, ...)
# comes from the vendor SDK (Solarflare/AMD Onload), not FetchContent — it's
# a NIC driver + userspace stack install, like jemalloc/liburing below.
# Without it this module still builds: sockets are plain, unaccelerated BSD
# sockets, just without stack isolation or spin-poll tuning.
if(UNIX AND NOT APPLE)
    include(CheckIncludeFileCXX)
    check_include_file_cxx("onload/extensions.h" MARKETLIB_HAVE_ONLOAD_HEADER)
    find_library(ONLOAD_EXT_LIB onload_ext)

    if(MARKETLIB_HAVE_ONLOAD_HEADER AND ONLOAD_EXT_LIB)
        target_compile_definitions(handlers_onload PUBLIC MARKETLIB_WITH_ONLOAD)
        target_link_libraries(handlers_onload PUBLIC ${ONLOAD_EXT_LIB})
        message(STATUS "Onload extensions SDK found — kernel-bypass tuning enabled")
    else()
        message(STATUS "Onload SDK not found — building with plain (unaccelerated) sockets")
    endif()
endif()
