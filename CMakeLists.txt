cmake_minimum_required(VERSION 3.16)
project(tachyon CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(tachyon_core STATIC
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(tachyon_core PUBLIC include)

add_executable(tachyon apps/main.cpp)
target_link_libraries(tachyon PRIVATE tachyon_core)

add_executable(tachyon_stream apps/stream_demo.cpp)
target_link_libraries(tachyon_stream PRIVATE tachyon_core)

option(TACHYON_BUILD_TESTS "Build unit tests" ON)
if(TACHYON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(TACHYON_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
if(TACHYON_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
