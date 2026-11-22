cmake_minimum_required(VERSION 3.20)
project(cxl_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(cxl_client STATIC
    src/cxl/cxl_client.cpp
)
target_include_directories(cxl_client PUBLIC src)
target_link_libraries(cxl_client PUBLIC Threads::Threads)

add_library(cxl_memory STATIC
    src/memory/software_cache.cpp
    src/memory/tiered_array.cpp
)
target_include_directories(cxl_memory PUBLIC src)
target_link_libraries(cxl_memory PUBLIC cxl_client)

add_library(cxl_workloads STATIC
    src/workloads/spmv.cpp
)
target_include_directories(cxl_workloads PUBLIC src)
target_link_libraries(cxl_workloads PUBLIC cxl_memory)

add_executable(serial_spmv apps/serial_spmv.cpp)
target_link_libraries(serial_spmv PRIVATE cxl_workloads)
