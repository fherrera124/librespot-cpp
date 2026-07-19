#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cspot {
struct TrackReference {
  TrackReference();

  // Resolved track GID
  std::vector<uint8_t> gid;
  std::string uri;

  // Type identifier
  enum class Type { TRACK, EPISODE };

  Type type;

  void decodeURI();

  bool operator==(const TrackReference& other) const;

  // Inverse of decodeURI(): builds "spotify:track:..."/"spotify:episode:..."
  // from a raw GID. Used to report a real track URI in connect-state PUTs
  // (docs/dealer_websocket_migration.md, Fase 5 - bridging real playback
  // events).
  static std::string encodeURI(const std::vector<uint8_t>& gid,
                               bool isEpisode = false);
};
}  // namespace cspot
