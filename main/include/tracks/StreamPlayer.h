#pragma once

#include <functional>
#include <optional>

#include "FileProvider.h"
#include "api/ApClient.h"
#include "api/SpClient.h"
#include "bell/utils/Semaphore.h"
#include "bell/utils/Task.h"
#include "events/EventModels.h"
#include "tracks/AudioDecoder.h"

namespace cspot {

// Fires synchronously (not posted through EventLoop) whenever
// announceState() has a new isPlaying/isPaused/isBuffering snapshot ready -
// see announceState()'s own comment for why this must be a direct call
// rather than a queued event. Defaults to a no-op the same way this
// codebase's other injected callbacks (VolumeChangedCallback,
// AudioOutputCallback) do.
using PlayerStateAnnounceCallback = std::function<void(const PlayerStateUpdate&)>;

class StreamPlayer : public bell::Task {
 public:
  StreamPlayer(
      std::shared_ptr<cspot::EventLoop> eventLoop,
      std::unique_ptr<cspot::FileProvider> fileProvider,
      std::unique_ptr<cspot::AudioDecoder> audioDecoder,
      PlayerStateAnnounceCallback playerStateAnnounceCallback =
          [](const PlayerStateUpdate&) {});

  ~StreamPlayer() override;

 private:
  const char* LOG_TAG = "StreamPlayer";

  std::shared_ptr<cspot::EventLoop> eventLoop;
  std::shared_ptr<cspot::SpClient> spClient;
  std::shared_ptr<cspot::ApClient> apClient;
  std::unique_ptr<cspot::FileProvider> fileProvider;
  PlayerStateAnnounceCallback playerStateAnnounceCallback;
  bell::Semaphore queueUpdateSemaphore;

  std::recursive_mutex playbackMutex;

  // What's playing (or about to). TrackQueueHandler is the sole authority
  // on *which* track this is - StreamPlayer only gets told, via
  // handleQueueUpdate(), never decides order itself.
  std::optional<SpotifyId> currentTrackId;
  std::optional<ProvidedFile> currentFile;

  bool flushRequested = false;
  bool isPlaying = false;

  std::unique_ptr<AudioDecoder> audioDecoder;

  void taskLoop() override;

  void registerHandlers();
  void handleQueueUpdate(const TrackQueueUpdate& queueUpdate);
  void handleFileProvided(const ProvidedFile& providedFile);
  bool isCurrentTrackReady();
  void handlePlayEvent(bool play);
  void handleFlushEvent();

  // Idempotent: opens the decoder for the current queue head once it's not
  // already open and the file's ready - deliberately regardless of
  // isPlaying (a paused transfer still needs to reach a real "ready"
  // state, not stay stuck "buffering" forever - see the .cpp). Safe to
  // call speculatively from handleFileProvided/handlePlayEvent/taskLoop:
  // playbackMutex is recursive, so nested calls from a caller already
  // holding it are fine.
  void maybeStartCurrentTrack();

  // Announces PlayerState.isPlaying/isPaused/isBuffering to the server via
  // playerStateAnnounceCallback (see ConnectStateHandler::onPlayerStateUpdate,
  // wired in directly by Session.cpp - not through EventLoop). Direct,
  // synchronous callback, dispatched before this function returns.
  //
  // isPlaying=false claims no session is loaded at all - callers must only
  // pass isBuffering=true (which implies isPlaying=false) once, for the
  // very first track, not merely once a CDN url/decrypt key are resolved.
  //
  // isPlaying itself is derived as !isBuffering, NOT from this class's own
  // isPlaying member (that member is the local decode gate, toggled by
  // pause/resume). Reporting it directly as PlayerState.isPlaying would
  // conflate "a session is loaded" with "audio is currently flowing",
  // making a paused device read as having nothing loaded in the real app -
  // isPaused is what reports the local decode gate instead.
  //
  // playbackId: only meaningful (and only ever passed) on the isBuffering=
  // false call - a fresh random id per track, never reused. nullopt on the
  // earlier isBuffering=true announce, since it isn't known yet;
  // ConnectStateHandler leaves the previous value untouched in that case
  // rather than clearing it.
  void announceState(bool isBuffering,
                     std::optional<std::string> playbackId = std::nullopt);
};
}  // namespace cspot
