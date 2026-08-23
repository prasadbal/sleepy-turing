add_library(core_orderbook INTERFACE)

target_include_directories(core_orderbook INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(core_orderbook INTERFACE core_options core_lockfree core_hashmap)

if(UNIX AND NOT APPLE)
    find_library(RT_LIB rt)
    if(RT_LIB)
        target_link_libraries(core_orderbook INTERFACE ${RT_LIB})
    endif()
endif()
