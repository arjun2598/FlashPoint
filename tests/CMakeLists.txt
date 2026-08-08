# GoogleTest is fetched at configure time and pinned to an exact release tag.
include(FetchContent)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE # avoids cloning GoogleTest's full history
    SYSTEM
    EXCLUDE_FROM_ALL)

# Keep GoogleTest from installing itself alongside FlashPoint, and (on Windows)
# from linking a different CRT than the rest of the build.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(flashpoint_tests
    order_book_test.cpp
    order_test.cpp
    types_test.cpp
    version_test.cpp)

target_link_libraries(flashpoint_tests
    PRIVATE
        FlashPoint::flashpoint
        flashpoint_warnings
        GTest::gtest_main)

set_target_properties(flashpoint_tests PROPERTIES CXX_EXTENSIONS OFF)

gtest_discover_tests(flashpoint_tests)
