#pragma once

#include <system_error>
#include "tl/expected.hpp"

namespace bell {
// Type alias for Result, which is an expected type that can either hold a value of type T or an error code.
template <typename T = void>
using Result = tl::expected<T, std::error_code>;

// Helper function to create a tl::expected object with an error code from an std::errc
template <typename T = void>
inline Result<T> make_unexpected_errc(const std::errc& err) {
  return tl::unexpected<std::error_code>(std::make_error_code(err));
}
}  // namespace bell
