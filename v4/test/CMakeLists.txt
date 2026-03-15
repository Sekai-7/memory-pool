include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(memory_pool_test memory_pool_test.cc)
target_link_libraries(memory_pool_test PRIVATE memory_pool_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(memory_pool_test)