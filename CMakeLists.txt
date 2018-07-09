cmake_minimum_required(VERSION 3.2)
add_definitions(-fPIC)
add_definitions(-std=c++11)
add_definitions(-g)
add_definitions(-Wall -Wextra)

set(LIBRARIES ${LIBRARIES} c)

# What to build: rmc-driver, rmc-daemon, libsonuma
set(LIBSONUMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libsonuma)

set (SON_TARGETS
    softrmc_daemon
    softrmc_driver
    libsonuma)

add_library(sonuma SHARED ${LIBSONUMA_DIR}/sonuma_daemon.cpp)
target_include_directories(sonuma PUBLIC ${LIBSONUMA_DIR})
