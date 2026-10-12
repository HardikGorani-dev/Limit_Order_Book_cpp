# 1. DOWNLOAD THE TESTING FRAMEWORK
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# 2. THE TEST BRICK (Target)
add_executable(lob_test test_orderbook.cpp)

# 3. SNAP THE BRICKS TOGETHER
target_link_libraries(lob_test PRIVATE 
    lob_core 
    GTest::gtest_main
)

# 4. REGISTER THE TESTS
include(GoogleTest)
gtest_discover_tests(lob_test)