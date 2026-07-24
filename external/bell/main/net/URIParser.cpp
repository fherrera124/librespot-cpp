#include "bell/net/URIParser.h"

// Standar includes
#include <cctype>
#include <string>

using namespace bell::net;

namespace {
const std::string hexDigitsStr = "0123456789ABCDEF";

char hex2int(const char* str) {
  unsigned char res = 0;
  for (int i = 0; i < 2; ++i) {
    char c = std::toupper(static_cast<unsigned char>(str[i]));
    int v = 0;
    if (c >= '0' && c <= '9') {
      v = c - '0';  // Convert '0'-'9' to 0-9
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;  // Convert 'A'-'F' to 10-15
    } else {
      // Invalid character
      return 0;
    }
    res = static_cast<unsigned char>((res << 4) | v);
  }
  return static_cast<char>(res);
}
};  // namespace

std::string bell::net::encodeURLEncoded(const std::string_view value) {
  std::string result;
  result.reserve(value.size() * 3);

  const std::string unreserved = "-_.~";
  for (char ch : value) {
    if (std::isalnum(static_cast<unsigned char>(ch)) ||
        unreserved.find(ch) != std::string::npos) {
      result.push_back(ch);
    } else {
      unsigned char uch = static_cast<unsigned char>(ch);
      result.push_back('%');
      result.push_back(hexDigitsStr[uch >> 4]);
      result.push_back(hexDigitsStr[uch & 0xF]);
    }
  }
  return result;
}

std::string bell::net::decodeURLEncoded(const std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    char ch = value[i];
    if (ch == '%' && i + 2 < value.size() && std::isxdigit(value[i + 1]) &&
        std::isxdigit(value[i + 2])) {
      char dec = hex2int(value.substr(i + 1).data());
      result.push_back(dec);
      i += 2;
    } else {
      result.push_back(ch);
    }
  }
  return result;
}

std::optional<bell::net::URI> bell::net::parseURI(std::string_view uri) {
  bell::net::URI result;

  // Parse the scheme
  auto schemeEnd = uri.find("://");
  if (schemeEnd == std::string_view::npos || schemeEnd == 0) {
    return std::nullopt;  // No scheme found or scheme is empty
  }

  result.scheme = std::string(uri.substr(0, schemeEnd));
  uri.remove_prefix(schemeEnd + 3);

  // Parse the authority part (host and port)
  auto authorityEnd = uri.find_first_of("/?");
  if (authorityEnd == std::string_view::npos)
    authorityEnd = uri.length();  // End of string, if no path/query present

  auto authority = uri.substr(0, authorityEnd);

  auto colonPos = authority.find(':');
  if (colonPos != std::string_view::npos) {
    // Host is mandatory, port is optional
    if (colonPos == 0 || colonPos == authority.length() - 1) {
      return std::nullopt;  // Malformed authority
    }
    result.host = std::string(authority.substr(0, colonPos));

    try {
      result.port = std::stoi(std::string(authority.substr(colonPos + 1)));
    } catch (const std::exception&) {
      return std::nullopt;  // Invalid port
    }
  } else {
    if (authority.empty()) {
      return std::nullopt;  // Missing host
    }
    result.host = std::string(authority);
  }

  uri.remove_prefix(std::min(authorityEnd, uri.size()));

  // Parse the path
  if (!uri.empty() && uri.front() == '/') {
    auto pathEnd = uri.find('?');
    if (pathEnd != std::string_view::npos) {
      result.path = std::string(uri.substr(0, pathEnd));
      uri.remove_prefix(pathEnd);
    } else {
      result.path = std::string(uri);
    }
  } else {
    result.path = "/";  // Default path if none specified
  }

  // Parse the query
  if (!uri.empty() && uri.front() == '?') {
    uri.remove_prefix(1);
    if (uri.empty()) {
      return std::nullopt;  // Query starts with '?' but is empty
    }
    result.query = std::string(uri);
  }

  return result;
}
