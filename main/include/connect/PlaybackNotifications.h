#pragma once

#include <cstdint>
#include <functional>
#include <variant>

#include "events/EventModels.h"

namespace cspot {

enum class PlaybackNotification { TrackChanged, PlayPauseChanged, Seeked };

using PlaybackNotificationData =
    std::variant<std::monostate, TrackMetadata, bool, uint32_t>;

struct PlaybackNotificationEvent {
  PlaybackNotification type;
  PlaybackNotificationData data;
};

// Fires on the shared EventLoop's own dispatch thread - the same one that
// processes remote Spotify commands (see ConnectStateHandler's
// LOCAL_TRACK_CHANGED/LOCAL_PLAY_PAUSE_CHANGED/LOCAL_SEEKED handlers for
// exactly where). Keep this fast/non-blocking: a slow callback delays
// everything else that thread has to process next, not just future
// notifications. Defaults to a no-op so callers that don't care about
// playback notifications don't need to pass anything.
using PlaybackNotificationCallback =
    std::function<void(const PlaybackNotificationEvent&)>;

}  // namespace cspot
