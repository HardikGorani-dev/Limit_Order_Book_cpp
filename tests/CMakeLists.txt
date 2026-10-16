include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(lob_test test_orderbook.cpp)

target_link_libraries(lob_test PRIVATE 
    lob_core 
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(lob_test)

add_executable(fuzz_orderbook fuzz_orderbook.cpp)
target_link_libraries(fuzz_orderbook PRIVATE lob_core)