#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "api/SpClient.h"
#include "bell/Result.h"
#include "events/EventLoop.h"
#include "proto/ConnectPb.h"

namespace cspot {

// Outcome of skipToNextTrack(): Advanced means the current track/index
// moved forward within the existing queue/context; WrappedToStart means
// either the context ran out and the cursor was reset to its first
// track, or there was nothing to advance to at all - callers decide
// whether that counts as a real advance (repeat-context).
enum class TrackAdvanceResult { Advanced, WrappedToStart };

class TrackQueueHandler {
 public:
  virtual ~TrackQueueHandler() = default;

  // embeddedPages: seeds the context from pages/tracks a play/transfer
  // command already carried, skipping a fetch. Left empty, fetches as
  // usual.
  virtual bell::Result<> loadContext(
      const std::string& contextUri,
      std::optional<std::string> currentTrackUri = std::nullopt,
      std::optional<std::string> currentTrackUid = std::nullopt,
      std::optional<uint32_t> currentTrackIndex = std::nullopt,
      const std::vector<cspot_proto::ContextPage>& embeddedPages = {}) = 0;

  virtual void setQueue(
      const std::vector<cspot_proto::ContextTrack>& queue) = 0;

  virtual void setPlayingQueue(bool isPlayingQueue) = 0;

  // Appends a single track to the end of the manual queue. Callers must
  // assign a uid first if the track doesn't already have one.
  virtual void addToQueue(const cspot_proto::ContextTrack& track) = 0;

  // Replaces the manual queue with queuedTracksInOrder: keeps the
  // currently-playing entry (queue[0] when isPlayingQueue) and appends
  // the rest after it.
  //
  // contextTracksInOrder is the other thing a remote set_queue can mean: a
  // drag-reorder of the upcoming "Next from: <context>" tracks (no
  // is_queued flag), possibly including a track foreign to this context.
  // Absorbed into the queue in the given order; contextIndex is caught up
  // once the real context tracks among them finish playing.
  virtual void reorderQueue(
      const std::vector<cspot_proto::ContextTrack>& queuedTracksInOrder,
      const std::vector<cspot_proto::ContextTrack>& contextTracksInOrder =
          {}) = 0;

  virtual std::optional<cspot_proto::ProvidedTrack> currentTrack() = 0;

  virtual std::optional<cspot_proto::ContextIndex> currentContextIndex() = 0;

  // targetTrackUri/targetTrackUid: when both are empty, advances one
  // position (queue pop, or context index + 1). When either is set,
  // searches the exposed next-tracks window (queue entries first, then
  // context) for a match and jumps straight there instead - this is how a
  // remote "skip_next" naming an explicit track (e.g. clicking an item in
  // the client's Queue panel) is expected to behave. A target not found in
  // that window (stale client state) falls back to WrappedToStart, same as
  // running off the end of the context.
  virtual bell::Result<TrackAdvanceResult> skipToNextTrack(
      const std::string& targetTrackUri = "",
      const std::string& targetTrackUid = "") = 0;

  virtual bell::Result<> skipToPreviousTrack(
      const std::string& trackUri = "") = 0;

  virtual bell::Result<> enableShuffle(bool shuffle) = 0;

  virtual tcb::span<cspot_proto::ProvidedTrack> nextTracks() = 0;
  virtual tcb::span<cspot_proto::ProvidedTrack> previousTracks() = 0;

  // forceNotify bypasses the "did anything actually change" dedup in the
  // default implementation - needed when a caller knows a fresh
  // QUEUE_UPDATED must go out even though the current track's identity
  // itself didn't change (e.g. repeat-track restarting the same track).
  virtual void updateTrackWindows(bool forceNotify = false) = 0;

  // Clears any previously loaded context (pages, cursor, target-track
  // search state, both windows) without touching the queue. Callers
  // entering a queue-only or ad-hoc-track session (no real context to
  // load) must call this first - otherwise a context left over from an
  // earlier transfer in the same session stays readable by
  // currentTrack()/currentContextIndex() even though nothing referencing
  // it was set up for this session.
  virtual void clearContext() = 0;
};

std::unique_ptr<TrackQueueHandler> createDefaultTrackQueueHandler(
    std::shared_ptr<SpClient> spClient, std::shared_ptr<EventLoop> eventLoop);
};  // namespace cspot
