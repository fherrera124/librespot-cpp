#pragma once

// Standard includes
#include <system_error>

namespace bell::mdns {
// Enum class for error codes
enum class MdnsErrc {
  success = 0,
  service_discovery_failed,
  service_registration_failed,
  resolution_failed,
  invalid_parameters,
  network_unavailable,
  name_conflict,
  unknown
};

// Mdns error category
class mdns_errc_category : public std::error_category {
 public:
  const char* name() const noexcept override { return "mdns"; }
  std::string message(int ev) const override {
    switch (static_cast<MdnsErrc>(ev)) {
      case MdnsErrc::success:
        return "success";
      case MdnsErrc::service_discovery_failed:
        return "service discovery failed";
      case MdnsErrc::service_registration_failed:
        return "service registration failed";
      case MdnsErrc::resolution_failed:
        return "resolution failed";
      case MdnsErrc::invalid_parameters:
        return "invalid parameters";
      case MdnsErrc::network_unavailable:
        return "network unavailable";
      default:
        return "unknown error";
    }
  }
};

// Error code category instance
inline const mdns_errc_category& mdns_category() {
  static mdns_errc_category c;
  return c;
}

// Create an error code with the mdns_category type
inline std::error_code make_error_code(const MdnsErrc& e) {
  return {static_cast<int>(e), mdns_category()};
}
}  // namespace bell::mdns

namespace std {
// Required so the enum can be used with std::error_code
template <>
struct is_error_code_enum<bell::mdns::MdnsErrc> : true_type {};
}  // namespace std
