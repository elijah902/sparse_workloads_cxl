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

add_executable(cxl_client_smoke tests/cxl_client_smoke.cpp)
target_link_libraries(cxl_client_smoke PRIVATE cxl_client)

enable_testing()
add_test(NAME cxl_client_smoke COMMAND cxl_client_smoke)
set_tests_properties(cxl_client_smoke PROPERTIES SKIP_RETURN_CODE 77)
