cmake_minimum_required(VERSION 3.20)
project(NanoBook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(NANOBOOK_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
                          -Wold-style-cast -Werror)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
elseif(MSVC)
    set(NANOBOOK_WARNINGS /W4 /WX)
endif()

add_library(nanobook STATIC src/order_book.cpp)
target_include_directories(nanobook PUBLIC include)
target_compile_options(nanobook PRIVATE ${NANOBOOK_WARNINGS})

add_executable(nanobook_tests tests/test_order_book.cpp)
target_link_libraries(nanobook_tests PRIVATE nanobook)
target_compile_options(nanobook_tests PRIVATE ${NANOBOOK_WARNINGS})

add_executable(nanobook_bench benchmarks/bench_latency.cpp)
target_link_libraries(nanobook_bench PRIVATE nanobook)
target_compile_options(nanobook_bench PRIVATE ${NANOBOOK_WARNINGS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nanobook_bench PRIVATE -march=native)
endif()

add_executable(nanobook_demo demo/nanobook_demo.cpp)
target_link_libraries(nanobook_demo PRIVATE nanobook)
target_compile_options(nanobook_demo PRIVATE ${NANOBOOK_WARNINGS})

enable_testing()
add_test(NAME nanobook_tests COMMAND nanobook_tests)
