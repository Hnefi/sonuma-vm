cmake_minimum_required(VERSION 3.2)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

set(LIBRARIES ${LIBRARIES} c pthread rt)

# What to build: rmc-driver, rmc-daemon, libsonuma
set(LIBSONUMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libsonuma)
set(RMCDAEMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/softrmc_daemon)
set(RMCDRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/softrmc_driver)

set(LIBSON_SOURCES
    ${LIBSONUMA_DIR}/sonuma_daemon.cpp
    ${LIBSONUMA_DIR}/sonuma_trampoline.c
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
    )

# RMC Daemon
add_executable(rmcd ${RMCDAEMON_DIR}/rmcd.cpp ${RMCDAEMON_DIR}/son_msg.cpp)
add_dependencies(rmcd rmc.ko) # fake dep just to build everything
target_compile_options(rmcd PUBLIC -D_GNU_SOURCE) # For sys/ipc.h
target_include_directories(rmcd PUBLIC ${LIBSONUMA_DIR})
