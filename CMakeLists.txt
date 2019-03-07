cmake_minimum_required(VERSION 3.9)
project(threadmon_task)

set(CMAKE_CXX_STANDARD 14)

set(LIBS pthread)

add_executable(threadmon_task
        main.cpp)

target_link_libraries(threadmon_task ${LIBS})