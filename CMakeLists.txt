cmake_minimum_required(VERSION 3.20)
project(LimitOrderBook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose Release or Debug" FORCE)
endif()

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ENABLE_NATIVE_ARCH "enable -march=native optimization " OFF)

add_library(lob_core src/OrderBook.cpp)

target_include_directories(lob_core PUBLIC include) 
    

if (MSVC)
    target_compile_options(lob_core PRIVATE /W4)
    if(WARNINGS_AS_ERRORS)
        target_compile_options(lob_core PRIVATE /WX)
    endif()
else()
    target_compile_options(lob_core PRIVATE 
        -Wall -Wextra -Wpedantic -fno-omit-frame-pointer
    )
    if(WARNINGS_AS_ERRORS)
        target_compile_options(lob_core PRIVATE -Werror)
    endif()
    
 if(ENABLE_NATIVE_ARCH)
        target_compile_options(lob_core PRIVATE -march=native)
    endif()
endif()

add_executable(lob_benchmark benchmarks/benchmark.cpp)
target_link_libraries(lob_benchmark PRIVATE lob_core)
if(NOT MSVC)
    target_compile_options(lob_benchmark PRIVATE -Wall -Wextra -Wpedantic -fno-omit-frame-pointer)
    if(WARNINGS_AS_ERRORS)
        target_compile_options(lob_benchmark PRIVATE -Werror)
    endif()
    if(ENABLE_NATIVE_ARCH)
        target_compile_options(lob_benchmark PRIVATE -march=native)
    endif()
endif()

enable_testing()
add_subdirectory(tests)