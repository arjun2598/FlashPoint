#include "flashpoint/version.hpp"

#include <string_view>

namespace flashpoint {

std::string_view version() noexcept {
    return kVersionString;
}

}  // namespace flashpoint
