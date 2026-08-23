add_executable(md_dispatch main.cpp)

target_include_directories(md_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(md_dispatch PRIVATE
    compiler_options
    core_logging
    core_threadpool
    core_orderbook
    core_perfmeasure
    handlers_onload
)
