#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cspot {
enum class SpotifyIdType : std::uint8_t { Track, Episode, Playlist };
struct SpotifyId {
  // GID constructor
  SpotifyId(SpotifyIdType type, const std::vector<std::byte>& gid);
  SpotifyId(SpotifyIdType type, const std::array<std::byte, 16>& gid);

  // Base62 GID constructor
  SpotifyId(SpotifyIdType type, const std::string& base62Gid);

  // URI constructor
  SpotifyId(const std::string& uri);

  // Default constructor
  SpotifyId() = default;

  // Guesses the type of the Spotify ID based on the URI context
  static SpotifyIdType getTypeFromContext(const std::string& contextUri);

  std::string hexGid() const;

  SpotifyIdType type;
  std::array<std::byte, 16> gid{};  // GID is always 16 bytes
  std::string base62Gid;            // Base62 GID representation
  std::string uri;                  // Full URI representation

  // Implement compare operators
  bool operator==(const SpotifyId& other) const {
    return type == other.type && gid == other.gid;
  }
};
}  // namespace cspot

// Implement a hash function for SpotifyId
namespace std {
template <>
struct hash<cspot::SpotifyId> {
  std::size_t operator()(const cspot::SpotifyId& id) const {
    return std::hash<std::string>{}(id.hexGid());
  }
};
}  // namespace std
