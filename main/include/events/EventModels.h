#pragma once

#include <optional>
#include "proto/MetadataPb.h"
#include "proto/SpotifyId.h"

namespace cspot {
struct CurrentTrackMetadata {
  SpotifyId trackId;
  std::string name;
  int32_t durationMs = 0;
};

struct AudioKeyResponse {
  bool success = false;
  SpotifyId trackId;
  std::vector<std::byte> fileId;
  std::vector<std::byte> audioKey;
};

struct TrackQueueUpdate {
  std::optional<SpotifyId> previousTrackId;
  std::vector<SpotifyId> nextTracksInQueue;
};

struct ProvidedFile {
  SpotifyId itemId{};
  std::optional<cspot_proto::Track> trackMetadata = std::nullopt;
  std::optional<cspot_proto::Episode> episodeMetadata = std::nullopt;
  std::string cdnUrl{};
  std::vector<std::byte> fileId{};
  std::vector<std::byte> decryptionKey{};
  bool isError = false;
};
};  // namespace cspot
