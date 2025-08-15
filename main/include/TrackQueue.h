#pragma once

#include "ContextTrackResolver.h"
#include "SessionContext.h"
#include "api/SpClient.h"
#include "proto/ConnectPb.h"
#include "proto/SpotifyId.h"

namespace cspot {

/**
 * @brief Manages the current track and context, providing methods to load tracks,
 * skip tracks, and retrieve next/previous tracks.
 */
class TrackQueue {
 public:
  TrackQueue(std::shared_ptr<SessionContext> sessionContext,
             std::shared_ptr<SpClient> spClient);
  /**
   * @brief Sets the queue for the track provider.
   */
  void setQueue(const cspot_proto::Queue& queue);

  /**
   * @brief sets the current track and context. This will completely replace the current context and track, and reset the queue.
   */
  bell::Result<> loadTrackAndContext(std::optional<std::string> trackUid,
                                     std::optional<std::string> trackUri,
                                     const cspot_proto::Context& context);

  std::optional<cspot_proto::ProvidedTrack> currentTrack();

  std::optional<cspot_proto::ContextIndex> currentContextIndex();

  void lockQueueMutex(bool lock);

  bell::Result<> skipToNextTrack(cspot_proto::ContextTrack* track = nullptr);

  bell::Result<> skipToPreviousTrack(
      cspot_proto::ContextTrack* track = nullptr);

  tcb::span<cspot_proto::ProvidedTrack> peekNextTracks();

  // Nanopb callback for encoding next tracks in the playback state
  static bool pbEncodeNextTracks(pb_ostream_t* stream, const pb_field_t* field,
                                 void* const* arg) {
    return static_cast<TrackQueue*>(*arg)->encodePbTracks(stream, field, false);
  }

  // Nanopb callback for encoding previous tracks in the playback state
  static bool pbEncodePreviousTracks(pb_ostream_t* stream,
                                     const pb_field_t* field,
                                     void* const* arg) {
    return static_cast<TrackQueue*>(*arg)->encodePbTracks(stream, field, true);
  }

 private:
  const char* LOG_TAG = "TrackQueue";

  std::shared_ptr<SessionContext> sessionContext;
  std::shared_ptr<SpClient> spClient;
  std::unique_ptr<ContextTrackResolver> contextTrackResolver;

  // Whether we are currently playing a queue
  bool isPlayingQueue = false;

  // Contains manually queued tracks, outside of the context
  std::vector<cspot_proto::ContextTrack> trackQueue;

  std::vector<cspot_proto::ProvidedTrack> previousTracks;
  std::vector<cspot_proto::ProvidedTrack> nextTracks;

  std::mutex queueMutex;

  // Index of the current track in the track queue
  uint32_t trackQueueIndex = 0;

  bool encodePbTracks(pb_ostream_t* stream, const pb_field_t* field,
                      bool isPreviousTracks);
};
}  // namespace cspot
