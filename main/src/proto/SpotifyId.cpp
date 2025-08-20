#include "proto/SpotifyId.h"

#include <crypto/Base62.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace cspot;

namespace {
// Returns uri-prefix based on SpotifyIdType
const char* typeToPrefix(SpotifyIdType type) {
  switch (type) {
    case SpotifyIdType::Track:
      return "spotify:track:";
    case SpotifyIdType::Episode:
      return "spotify:episode:";
    case SpotifyIdType::Playlist:
      return "spotify:playlist:";
    default:
      throw std::invalid_argument("Unknown SpotifyIdType");
  }
}

// Converts a URI prefix to SpotifyIdType
SpotifyIdType uriToType(std::string_view prefix) {
  if (prefix.starts_with("spotify:track:")) {
    return SpotifyIdType::Track;
  }
  if (prefix.starts_with("spotify:episode:")) {
    return SpotifyIdType::Episode;
  }

  if (prefix.starts_with("spotify:playlist:")) {
    return SpotifyIdType::Playlist;
  }

  throw std::invalid_argument("Unknown Spotify URI prefix");
}
}  // namespace

cspot::SpotifyId::SpotifyId(SpotifyIdType type,
                            const std::vector<std::byte>& gid)
    : type(type) {
  if (gid.size() != 16) {
    throw std::invalid_argument("GID must be exactly 16 bytes long");
  }
  std::copy(gid.begin(), gid.end(), this->gid.begin());

  this->type = type;

  // Convert GID to Base62
  this->base62Gid = base62Encode(gid.data(), gid.size());
  // Pad Base62 GID to 22 characters
  this->base62Gid =
      std::string(22 - this->base62Gid.size(), '0') + this->base62Gid;

  // Construct URI based on type
  this->uri = typeToPrefix(type) + this->base62Gid;
}

cspot::SpotifyId::SpotifyId(SpotifyIdType type, const std::string& base62Gid)
    : type(type), base62Gid(base62Gid) {
  // Decode Base62 GID to bytes
  size_t gidSize = 16;
  if (!base62Decode(base62Gid, this->gid.data(), gidSize) || gidSize != 16) {
    throw std::invalid_argument("Invalid Base62 GID");
  }

  // Construct URI based on type
  this->uri = typeToPrefix(type) + base62Gid;
}

cspot::SpotifyId::SpotifyId(const std::string& uri)
    : type(uriToType(uri)), uri(uri) {
  // Extract Base62 GID from URI
  auto base62Start = uri.find(':', 8) + 1;  // Skip "spotify:" prefix
  this->base62Gid = uri.substr(base62Start);

  // Decode Base62 GID to bytes
  size_t gidSize = 16;
  if (!base62Decode(this->base62Gid, this->gid.data(), gidSize) ||
      gidSize != 16) {
    throw std::invalid_argument("Invalid Base62 GID in URI");
  }
}

std::string cspot::SpotifyId::hexGid() const {
  std::string hex;
  hex.reserve(32);  // 16 bytes * 2 hex digits per byte

  std::stringstream ss;
  ss << std::hex << std::setfill('0');  // Set hex output and pad with '0'

  for (const auto& byte : gid) {
    ss << std::setw(2)
       << static_cast<unsigned>(byte);  // Convert byte to int for stream output
  }
  hex = ss.str();
  return hex;
}

SpotifyIdType cspot::SpotifyId::getTypeFromContext(
    const std::string& contextUri) {
  if (contextUri.starts_with("spotify:episode:") ||
      contextUri.starts_with("spotify:show:")) {
    return SpotifyIdType::Episode;
  }

  return SpotifyIdType::Track;  // Default to Track for other contexts
}
