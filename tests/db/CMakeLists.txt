add_executable(test_db test_db.cpp test_diag.cpp test_query_descriptor.cpp)

# core_db must come AFTER core_testing. core_testing pulls in core_perfmeasure,
# which leaks -fno-exceptions, and the last flag on the command line wins --
# so core_db's reenable_exceptions has to land after it (the db layer throws).
# See the reenable_exceptions comment in cmake/compiler_options.cmake.
target_link_libraries(test_db PRIVATE core_testing core_db)
catch_discover_tests(test_db)
