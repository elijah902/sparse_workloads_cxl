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
    src/workloads/stencil.cpp
)
target_include_directories(cxl_workloads PUBLIC src)
target_link_libraries(cxl_workloads PUBLIC cxl_memory)

add_executable(cxl_client_smoke tests/cxl_client_smoke.cpp)
target_link_libraries(cxl_client_smoke PRIVATE cxl_client)

add_executable(tiered_array_test tests/tiered_array_test.cpp)
target_link_libraries(tiered_array_test PRIVATE cxl_memory)

add_executable(stencil_test tests/stencil_test.cpp)
target_link_libraries(stencil_test PRIVATE cxl_workloads)

add_executable(stencil_bench apps/stencil_bench.cpp)
target_link_libraries(stencil_bench PRIVATE cxl_workloads)

enable_testing()
add_test(NAME cxl_client_smoke COMMAND cxl_client_smoke)
set_tests_properties(cxl_client_smoke PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME tiered_array_test COMMAND tiered_array_test)
set_tests_properties(tiered_array_test PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME stencil_test COMMAND stencil_test)
set_tests_properties(stencil_test PROPERTIES SKIP_RETURN_CODE 77)
