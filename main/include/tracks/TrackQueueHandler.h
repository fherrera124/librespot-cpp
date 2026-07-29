#pragma once

#include <optional>
#include <string>

#include "api/SpClient.h"
#include "bell/Result.h"
#include "events/EventLoop.h"
#include "proto/ConnectPb.h"

namespace cspot {

// Outcome of skipToNextTrack(): Advanced means the current track/index
// moved forward within the existing queue/context; WrappedToStart means
// the context ran out and the cursor was reset to its first track -
// callers decide whether that counts as a real advance (repeat-context).
enum class TrackAdvanceResult { Advanced, WrappedToStart };

class TrackQueueHandler {
 public:
  virtual ~TrackQueueHandler() = default;

  virtual bell::Result<> loadContext(
      const std::string& contextUri,
      std::optional<std::string> currentTrackUri = std::nullopt,
      std::optional<std::string> currentTrackUid = std::nullopt) = 0;

  virtual void setQueue(
      const std::vector<cspot_proto::ContextTrack>& queue) = 0;

  virtual void setPlayingQueue(bool isPlayingQueue) = 0;

  virtual std::optional<cspot_proto::ProvidedTrack> currentTrack() = 0;

  virtual std::optional<cspot_proto::ContextIndex> currentContextIndex() = 0;

  virtual bell::Result<TrackAdvanceResult> skipToNextTrack(
      const std::string& trackUri = "") = 0;

  virtual bell::Result<> skipToPreviousTrack(
      const std::string& trackUri = "") = 0;

  virtual bell::Result<> enableShuffle(bool shuffle) = 0;

  virtual tcb::span<cspot_proto::ProvidedTrack> nextTracks() = 0;
  virtual tcb::span<cspot_proto::ProvidedTrack> previousTracks() = 0;

  // forceNotify bypasses the "did anything actually change" dedup below -
  // needed when a caller knows a fresh QUEUE_UPDATED must go out even
  // though the current track's identity itself didn't change (e.g.
  // repeat-track restarting the same track).
  virtual void updateTrackWindows(bool forceNotify = false) = 0;
};

std::unique_ptr<TrackQueueHandler> createDefaultTrackQueueHandler(
    std::shared_ptr<SpClient> spClient, std::shared_ptr<EventLoop> eventLoop);
};  // namespace cspot
