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
  // Per-context-instance identifier (context-resolve's own "uid" field) -
  // disambiguates duplicate tracks/local files within the same playlist,
  // where uri/gid alone can't. A remote "play" command's
  // options.skip_to.track_uid targets this, not uri - confirmed on real
  // hardware clicking a track inside an already-active playlist (no
  // track_uri/track_index in that payload at all). See
  // PlayerCommandHandler::handlePlay().
  std::string uid;

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
