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
  std::vector<SpotifyId> nextTracks;
  std::optional<SpotifyId> currentTrackId;
};

struct PlayerStateUpdate {
  // Whether a session is loaded at all - stays true while paused. NOT the
  // same as "audio is currently flowing" (see StreamPlayer::announceState
  // and isPaused below) - conflating the two ("isPlaying = !isPaused")
  // reports a paused device as having nothing loaded, which reads as
  // permanently stopped/disconnected in the real Spotify app.
  bool isPlaying;
  bool isPaused = false;
  bool isBuffering;
  int64_t timestamp;
  int64_t positionAsOfTimestamp;
  int64_t playbackDurationMs;
  // Fresh random id for this playback, hex-encoded, set only on the
  // isBuffering=false announce (empty on the earlier isBuffering=true one -
  // not known yet). See StreamPlayer::announceState()'s comment.
  std::string playbackId;
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
