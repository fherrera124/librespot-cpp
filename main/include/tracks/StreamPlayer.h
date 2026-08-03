#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "AudioSink.h"
#include "FileProvider.h"
#include "api/ApClient.h"
#include "api/SpClient.h"
#include "bell/utils/Semaphore.h"
#include "bell/utils/Task.h"
#include "events/EventModels.h"
#include "tracks/AudioDecoder.h"

namespace cspot {

// Called synchronously by announceState() - not posted through EventLoop.
// Defaults to a no-op.
using PlayerStateAnnounceCallback = std::function<void(const PlayerStateUpdate&)>;

class StreamPlayer : public bell::Task {
 public:
  StreamPlayer(
      std::shared_ptr<cspot::EventLoop> eventLoop,
      std::unique_ptr<cspot::FileProvider> fileProvider,
      std::unique_ptr<cspot::AudioDecoder> audioDecoder,
      PlayerStateAnnounceCallback playerStateAnnounceCallback =
          [](const PlayerStateUpdate&) {},
      std::shared_ptr<cspot::AudioSink> audioSink =
          std::make_shared<cspot::NullAudioSink>());

  ~StreamPlayer() override;

 private:
  const char* LOG_TAG = "StreamPlayer";

  std::shared_ptr<cspot::EventLoop> eventLoop;
  std::shared_ptr<cspot::SpClient> spClient;
  std::shared_ptr<cspot::ApClient> apClient;
  std::unique_ptr<cspot::FileProvider> fileProvider;
  PlayerStateAnnounceCallback playerStateAnnounceCallback;
  std::shared_ptr<cspot::AudioSink> audioSink;
  bell::Semaphore queueUpdateSemaphore;

  std::recursive_mutex playbackMutex;

  // TrackQueueHandler is the sole authority on track order; StreamPlayer
  // only receives it via handleQueueUpdate().
  std::optional<SpotifyId> currentTrackId;
  std::optional<ProvidedFile> currentFile;

  bool flushRequested = false;
  bool isPlaying = false;

  // Set on natural end-of-track; skips the next flushRequested's
  // audioSink->flush(), since that audio is the track's own tail, not
  // stale audio from pause/seek/skip/prev.
  bool suppressNextSinkFlush = false;

  // Set by handleSeekEvent() (EventLoop thread), applied by taskLoop()
  // (this class's own thread) - see handleSeekEvent()'s comment.
  std::optional<int64_t> pendingSeekMs;

  std::unique_ptr<AudioDecoder> audioDecoder;

  void taskLoop() override;

  void registerHandlers();
  void handleQueueUpdate(const TrackQueueUpdate& queueUpdate);
  void handleFileProvided(const ProvidedFile& providedFile);
  bool isCurrentTrackReady();
  void handlePlayEvent(bool play);
  void handleFlushEvent();
  void handleSeekEvent(int64_t positionMs);

  // Idempotent: opens the decoder once the file's ready, regardless of
  // isPlaying (a paused transfer still needs to leave "buffering" - see
  // the .cpp). Safe to call speculatively; playbackMutex is recursive.
  void maybeStartCurrentTrack();

  // Announces PlayerState to the server via playerStateAnnounceCallback
  // (ConnectStateHandler::onPlayerStateUpdate, wired by Session.cpp) - a
  // direct synchronous call, not through EventLoop.
  //
  // isBuffering=true must only be passed once, for the very first track -
  // it implies isPlaying=false, i.e. no session loaded at all.
  //
  // PlayerState.isPlaying is !isBuffering, not this class's isPlaying
  // member (the local pause/resume gate) - reporting that directly would
  // make a paused device look like nothing's loaded; isPaused reports the
  // local gate instead.
  //
  // playbackId: fresh per track, only passed on the isBuffering=false
  // call. nullopt while buffering (not known yet); ConnectStateHandler
  // leaves the previous value untouched then.
  //
  // positionMs: the caller's decision, not an assumption here - both
  // current callers pass 0, since a track just starting genuinely has no
  // position yet.
  void announceState(bool isBuffering,
                     std::optional<std::string> playbackId = std::nullopt,
                     int64_t positionMs = 0);
};
}  // namespace cspot
