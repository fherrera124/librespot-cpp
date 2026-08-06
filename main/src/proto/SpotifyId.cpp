#include "proto/SpotifyId.h"

#include <crypto/Base62.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "bell/Logger.h"

using namespace cspot;

namespace {
const char* LOG_TAG = "SpotifyId";

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
    : SpotifyId(type, gid.data(), gid.size()) {}

cspot::SpotifyId::SpotifyId(SpotifyIdType type,
                            const std::array<std::byte, 16>& gid)
    : SpotifyId(type, gid.data(), gid.size()) {}

cspot::SpotifyId::SpotifyId(SpotifyIdType type, const std::byte* gid,
                            size_t size)
    : type(type) {
  // Every real Spotify GID is 16 bytes; this->gid is a fixed array, so a
  // gid of any other length (malformed/corrupted metadata, protocol
  // drift) must never be allowed to copy more than 16 bytes into it -
  // that would be a buffer overflow, not just a wrong ID. Clamp instead
  // of trusting the caller.
  if (size != 16) {
    BELL_LOG(error, LOG_TAG,
             "Spotify GID has unexpected size {} (expected 16), truncating",
             size);
  }
  this->gid.fill(std::byte{0});
  std::copy(gid, gid + std::min(size, this->gid.size()), this->gid.begin());

  this->type = type;

  // Convert GID to Base62 (from the same clamped 16 bytes now in this->gid,
  // so base62Gid/uri stay consistent with gid rather than reflecting
  // whatever extra bytes the caller passed in).
  this->base62Gid = base62Encode(this->gid.data(), this->gid.size());
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
  if (!base62Decode(this->base62Gid, this->gid.data(), gidSize)) {
    throw std::invalid_argument("Invalid Base62 GID in URI");
  }

  if (gidSize < 16) {
    // Move gid right to fill leading zeros
    memmove(this->gid.data() + (16 - gidSize), this->gid.data(), gidSize);
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
      contextUri.starts_with("spotify:show:") ||
      contextUri.ends_with(":collection:your-episodes")) {
    return SpotifyIdType::Episode;
  }

  return SpotifyIdType::Track;  // Default to Track for other contexts
}
