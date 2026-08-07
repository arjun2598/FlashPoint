// Smoke tests for the build itself
#include "flashpoint/version.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace flashpoint {
namespace {

// Proves the linked library was built from the same headers we compiled
// against. A mismatch here means a stale build directory or a broken install.
TEST(Version, LinkedLibraryMatchesCompiledHeaders) {
    EXPECT_EQ(version(), kVersionString);
}

// Guards the configure_file() substitution. If CMake ever fails to expand the
// @PROJECT_VERSION@ placeholders, the string is empty or still contains '@'
// and this catches it rather than letting a malformed header ship.
TEST(Version, StringIsPopulatedAndWellFormed) {
    ASSERT_FALSE(kVersionString.empty());
    EXPECT_EQ(kVersionString.find('@'), std::string_view::npos);
    EXPECT_EQ(std::count(kVersionString.begin(), kVersionString.end(), '.'), 2);
}

// The version components must be the numeric form of the same string.
TEST(Version, ComponentsAreConsistentWithString) {
    const std::string expected = std::to_string(kVersionMajor) + '.' +
                                 std::to_string(kVersionMinor) + '.' +
                                 std::to_string(kVersionPatch);
    EXPECT_EQ(kVersionString, expected);
}

}  // namespace
}  // namespace flashpoint
