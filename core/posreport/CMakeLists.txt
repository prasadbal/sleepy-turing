# Header-only: everything lives in include/posreport/. No Oracle/db
# dependency at all -- Position/Instrument/db-lookup/config-lookup are all
# supplied by the caller as plain structs and std::function resolvers, so
# this compiles and is usable standalone (e.g. against a Position struct
# backed by anything, not necessarily Oracle).
add_library(core_posreport INTERFACE)

target_include_directories(core_posreport INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Not core_options: parse() throws std::runtime_error on a syntax error (by
# design -- a malformed column expression in a config file is a startup-time
# error, not a value to propagate through std::expected everywhere it's
# used), same reasoning as core_db/core_config linking compiler_options only.
target_link_libraries(core_posreport INTERFACE
    compiler_options
    Boost::pfr
    reenable_exceptions
)
