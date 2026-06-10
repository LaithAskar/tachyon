include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(tachyon_tests
    test_order_book.cpp
    test_spsc_queue.cpp
    test_threaded_matcher.cpp
    test_exchange.cpp
)
target_link_libraries(tachyon_tests
    PRIVATE
        tachyon_core
        GTest::gtest_main
)
# SPSC test needs the threading runtime.
find_package(Threads REQUIRED)
target_link_libraries(tachyon_tests PRIVATE Threads::Threads)

include(GoogleTest)
gtest_discover_tests(tachyon_tests)

add_test(NAME tachyon_cli_demo_smoke COMMAND tachyon)
set_tests_properties(tachyon_cli_demo_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "FINAL STATS"
)

add_test(NAME tachyon_stream_smoke COMMAND tachyon_stream 5 2 0)
set_tests_properties(tachyon_stream_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "\\\"event\\\":\\\"order\\\""
)
