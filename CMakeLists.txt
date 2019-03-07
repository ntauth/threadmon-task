cmake_minimum_required(VERSION 2.8)
project(threadmon_task)

set(CMAKE_CXX_STANDARD 14)

set(LIBS pthread rt)

add_executable(threadmon_task
        main.cpp)

target_link_libraries(threadmon_task ${LIBS})