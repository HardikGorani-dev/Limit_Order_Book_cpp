
cmake_minimum_required(VERSION 3.20)
project(LimitOrderBook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(lob_core src/OrderBook.cpp)

target_include_directories(lob_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if (MSVC)
    target_compile_options(lob_core PRIVATE /W4 /WX)
else()
    target_compile_options(lob_core PRIVATE 
        -Wall -Wextra -Wpedantic -Werror 
        
        -fno-omit-frame-pointer
    )
  endif()

enable_testing()
add_subdirectory(tests)
add_executable(lob_benchmark benchmarks/benchmark.cpp)
target_link_libraries(lob_benchmark PRIVATE lob_core)
if (NOT MSVC)
    target_compile_options(lob_benchmark PRIVATE -O3 -march=native)
endif()