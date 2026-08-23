add_library(core_networking INTERFACE)

target_include_directories(core_networking INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_networking INTERFACE core_options)

# Boost.Asio — add when concrete transports are implemented:
#   cmake_policy(SET CMP0167 NEW)
#   find_package(Boost CONFIG REQUIRED)
#   target_link_libraries(core_networking INTERFACE Boost::headers)

if(UNIX AND NOT APPLE)
    find_library(LIBURING_LIB uring)
    if(LIBURING_LIB)
        target_link_libraries(core_networking INTERFACE ${LIBURING_LIB})
        target_compile_definitions(core_networking INTERFACE SLEEPY_HAS_URING)
        message(STATUS "liburing found — io_uring transport enabled")
    else()
        message(STATUS "liburing not found — run: sudo apt install liburing-dev")
    endif()
endif()
