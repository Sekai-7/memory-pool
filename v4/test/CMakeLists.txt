include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

if(NOT MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS)
  message(FATAL_ERROR "memory_pool_test requires MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS=ON")
endif()

add_executable(memory_pool_test memory_pool_test.cc)
target_link_libraries(memory_pool_test PRIVATE memory_pool_core GTest::gtest_main)
target_compile_definitions(memory_pool_test PRIVATE MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS)

include(GoogleTest)
gtest_discover_tests(memory_pool_test)
