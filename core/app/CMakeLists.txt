add_library(core_app STATIC
    src/application.cpp
    src/server_application.cpp
    src/signals.cpp
)

target_include_directories(core_app PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(core_app PRIVATE
    ${backward_SOURCE_DIR}
)
# app uses exceptions (backward-cpp, CLI11 via cmdline) — compiler_options
# only. core_logging transitively leaks core_options' -fno-exceptions in
# here (see reenable_exceptions in cmake/compiler_options.cmake) — must be
# linked last so its override lands after the leaked flag.
target_link_libraries(core_app PUBLIC
    compiler_options
    core_config
    core_cmdline
    core_logging
    reenable_exceptions
)
add_backward(core_app)
# add_backward() sets core_app's private LINK_LIBRARIES property directly,
# which (for a STATIC library) does not propagate to consumers — re-expose
# the same libraries as a PUBLIC usage requirement so executables linking
# core_app (e.g. apps/demo) actually get -ldw/-lbfd/etc. on their link line.
target_link_libraries(core_app PUBLIC ${BACKWARD_LIBRARIES})
