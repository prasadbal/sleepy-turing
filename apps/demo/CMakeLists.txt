add_executable(demo main.cpp)

target_include_directories(demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(demo PRIVATE
    compiler_options
    core_app
    core_lockfree
    core_perfmeasure
)
