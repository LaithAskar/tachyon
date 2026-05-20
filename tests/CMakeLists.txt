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
