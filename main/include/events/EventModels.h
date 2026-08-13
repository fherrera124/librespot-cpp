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

// PLAYER_FLUSH's payload for a transfer: flush, seek, and isPlaying,
// applied atomically.
struct FlushResumeState {
  int64_t positionMs = 0;
  bool isPlaying = true;
};

// PLAYER_PLAY's payload. pausePositionMs is set only for a genuine local
// pause with an open decoder - StreamPlayer seeks back to it and flushes
// the sink immediately instead of draining the ring buffer on its own.
// Left nullopt for every other shouldPlay=false post (decoder not open
// yet, or deliberately left where it is).
struct PlayPauseCommand {
  bool shouldPlay = true;
  std::optional<int64_t> pausePositionMs;
};

struct TrackQueueUpdate {
  std::optional<SpotifyId> currentTrackId;
};

// TRACK_NEAR_END's answer: ConnectStateHandler's best guess at what
// becomes current next (repeat-track's target, or the head of
// TrackQueueHandler's next-tracks window). A hint, not authority -
// StreamPlayer only uses it to prefetch ahead of time, and re-validates
// identity against the real advance before adopting the result.
struct NextTrackHint {
  SpotifyId trackId;
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
  // cdnUrl is an episode's plain external_url (unencrypted HTTP, no
  // decryptionKey) rather than a Spotify-CDN AudioFile - routes to
  // AudioDecoder::openExternalStream() instead of openStream().
  bool isExternalUrl = false;
};
};  // namespace cspot
