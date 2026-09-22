# Header-only: everything lives in include/db/ (see README.md for the layout).
# There is nothing to compile into a library, so consumers compile it.
add_library(core_db INTERFACE)

target_include_directories(core_db INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Deliberately NOT core_options. The other core libs are no-exceptions and
# report errors via std::expected; the db layer reports failure by throwing
# (OciStatementStateError, the from_text() parse failures, ...), the same
# reason core_config and core_logging link compiler_options only. Not a
# hot-path component -- database access is batch/reporting work, not the
# sub-10us path -- so exceptions are acceptable here.
#
# reenable_exceptions: a consumer that has -fno-exceptions leaked in from
# some other core lib would otherwise fail to compile any `throw` in these
# headers. See cmake/compiler_options.cmake for how that leak happens and
# why it has to be linked last.
target_link_libraries(core_db INTERFACE
    compiler_options
    Boost::pfr
    reenable_exceptions
)

# ── Oracle client ────────────────────────────────────────────────────────────
# include/db/oracle/oci_compat.h picks the real <oci.h> when it is on the
# include path and otherwise falls back to a mock (oci_mock.h). That fallback
# is silent, which suits development and is dangerous for a real build: with
# no client installed everything still compiles, and it only fails -- or
# worse, "succeeds" against canned data -- at runtime. So make the choice
# explicit and visible instead of leaving it to whichever include path
# happens to be set.
set(ORACLE_OCI_INCLUDE_DIR "" CACHE PATH
    "Directory containing oci.h (Oracle Instant Client sdk/include)")
set(ORACLE_OCI_LIBRARY "" CACHE FILEPATH
    "Path to the Oracle client library (libclntsh)")

if(ORACLE_OCI_INCLUDE_DIR OR ORACLE_OCI_LIBRARY)
    if(NOT (ORACLE_OCI_INCLUDE_DIR AND ORACLE_OCI_LIBRARY))
        message(FATAL_ERROR
            "db: set both ORACLE_OCI_INCLUDE_DIR and ORACLE_OCI_LIBRARY, or neither")
    endif()
    target_include_directories(core_db INTERFACE ${ORACLE_OCI_INCLUDE_DIR})
    target_link_libraries(core_db INTERFACE ${ORACLE_OCI_LIBRARY})
    set(SLEEPY_DB_REAL_OCI ON)
    message(STATUS "db: building against the real Oracle client (${ORACLE_OCI_INCLUDE_DIR})")
else()
    set(SLEEPY_DB_REAL_OCI OFF)
    message(STATUS "db: no Oracle client configured -- building against the OCI mock; "
                   "nothing built here can reach a real database "
                   "(set ORACLE_OCI_INCLUDE_DIR and ORACLE_OCI_LIBRARY to change that)")
endif()

# ── Examples ─────────────────────────────────────────────────────────────────
# All built, so API drift against the headers shows up at compile time rather
# than the next time someone runs one. The live_* demos need a real Oracle
# client and a reachable database when *run*; they compile against the mock.
# None are registered as tests -- see tests/db for those.
foreach(example
        demo
        demo_client
        diag_demo
        live_oracle_demo
        live_oracle_client_demo
        live_oracle_empty_table_demo
        live_oracle_generic_demo)
    add_executable(db_${example} examples/${example}.cpp)
    target_link_libraries(db_${example} PRIVATE core_db)
endforeach()
