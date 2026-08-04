#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include "proto/MetadataPb.h"
#include "proto/SpotifyId.h"

namespace cspot {
// Display metadata for the outward "now playing" notification
// (PlaybackNotifications.h) - projected from cspot_proto::Track by
// StreamPlayer::announceState(), same fields master's TrackInfo flattens a
// Track/Episode proto into (src/TrackQueue.cpp).
struct TrackMetadata {
  std::string uri;
  std::string name;
  std::string artist;  // first artist only, matches master's TrackInfo
  std::string album;
  std::string imageUrl;
  uint32_t durationMs = 0;
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
  // isPlaying: same semantics as PlayerState.isPlaying (proto/ConnectPb.h).
  bool isPlaying;
  bool isPaused = false;
  bool isBuffering;
  int64_t timestamp;
  int64_t positionAsOfTimestamp;
  int64_t playbackDurationMs;
  // Fresh random id for this playback, hex-encoded, set only on the
  // isBuffering=false announce (nullopt on the earlier isBuffering=true
  // one - not known yet). See StreamPlayer::announceState()'s comment.
  std::optional<std::string> playbackId;
  // Display metadata for the outward TrackChanged notification - same
  // isBuffering=false-only timing as playbackId above (the real metadata
  // isn't resolved yet on the earlier announce).
  std::optional<TrackMetadata> trackMetadata = std::nullopt;
};

struct ProvidedFile {
  SpotifyId itemId{};
  std::optional<cspot_proto::Track> trackMetadata = std::nullopt;
  std::optional<cspot_proto::Episode> episodeMetadata = std::nullopt;
  std::string cdnUrl{};
  std::vector<std::byte> fileId{};
  std::vector<std::byte> decryptionKey{};
  // The Vorbis quality actually resolved (may be lower than requested if
  // the preferred format wasn't offered)
  AudioFormat format = AudioFormat_OGG_VORBIS_160;
  bool isError = false;
};
};  // namespace cspot
