cmake_minimum_required(VERSION 3.2)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    #top level project.
    set(CMAKE_C_COMPILER gcc)
    set(CMAKE_CXX_COMPILER g++)

    set(CMAKE_CXX_FLAGS "-std=c++14 -Wall -Wextra -pedantic -Wno-unused-function -march=native -Wno-unused-parameter -Wno-format -g")
    set(CMAKE_C_FLAGS "-std=c99 -Wall -Wextra -pedantic -Wno-unused-function -Wno-unused-parameter -Wno-format -march=native -g")
    set(REL_PATH "../build")
else()
    #i am called from sonofa_herd with subdirectory
    set(REL_PATH "../../build")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

set(LIBRARIES ${LIBRARIES} c pthread rt)

# What to build: rmc-driver, rmc-daemon, libsonuma
set(LIBSONUMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libsonuma)
set(RMCDAEMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/softrmc_daemon)
set(RMCDRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/softrmc_driver)
set(SAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sample_code)

set(LIBSON_SOURCES
    ${LIBSONUMA_DIR}/sonuma_daemon.cpp
    #${LIBSONUMA_DIR}/sonuma_trampoline.c
    )

set(SAMPLE_CODE_FILES
    bench_async
    bench_sync
    bench_server
    many_to_one
    rpc_service
    rpc_sync
    )

set(SCODE_CPP
    bench_async
    bench_sync
    bench_server
    many_to_one
    rpc_service
    rpc_sync
    )

#Libsonuma
add_library(sonuma SHARED ${LIBSON_SOURCES})
target_link_libraries(sonuma ${LIBRARIES})
target_compile_options(sonuma PUBLIC -D_GNU_SOURCE) # For sys/ipc.h
target_include_directories(sonuma PUBLIC ${LIBSONUMA_DIR})

# RMC Driver
ADD_CUSTOM_TARGET(
    rmc.ko
    COMMAND make 
    WORKING_DIRECTORY ${RMCDRIVER_DIR}
    VERBATIM
    )
ADD_CUSTOM_COMMAND(
    TARGET rmc.ko POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy 
    ${RMCDRIVER_DIR}/rmc.ko ${CMAKE_SOURCE_DIR}/build
    )

# RMC Daemon
add_executable(rmcd ${RMCDAEMON_DIR}/rmcd.cpp ${RMCDAEMON_DIR}/son_msg.cpp)
add_dependencies(rmcd rmc.ko) # fake dep just to build everything
target_compile_options(rmcd PUBLIC -D_GNU_SOURCE) # For sys/ipc.h
target_include_directories(rmcd PUBLIC ${LIBSONUMA_DIR})

# Sample code
#foreach(f ${SCODE_CPP})
#add_executable(${f} ${SAMPLE_DIR}/${f}.cpp)
#target_link_libraries(${f} sonuma rt)
#endforeach()
