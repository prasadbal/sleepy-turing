add_executable(test_lockfree
    test_spsc_queue.cpp
    test_mpsc_queue.cpp
    test_disruptor.cpp
)
target_link_libraries(test_lockfree PRIVATE core_lockfree core_testing)
catch_discover_tests(test_lockfree)
