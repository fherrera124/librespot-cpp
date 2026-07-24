#include "bell/http/Common.h"

#include <unordered_map>

using namespace bell;

namespace {
const std::unordered_map<std::string_view, http::Method> strMethodMap = {
    {"GET", http::Method::GET},   {"POST", http::Method::POST},
    {"PUT", http::Method::PUT},   {"DELETE", http::Method::DELETE},
    {"HEAD", http::Method::HEAD}, {"OPTIONS", http::Method::OPTIONS},
};
}

std::string_view http::methodToString(Method method) {
  for (const auto& [str, m] : strMethodMap) {
    if (m == method) {
      return str;
    }
  }

  return "INVALID";
}

http::Method http::parseMethod(std::string_view method) {
  auto it = strMethodMap.find(method);
  if (it != strMethodMap.end()) {
    return it->second;
  }

  return Method::INVALID;
}
