#pragma once

#include <unordered_map>

#include "FileProvider.h"
#include "api/ApClient.h"
#include "api/SpClient.h"
#include "bell/utils/Semaphore.h"
#include "bell/utils/Task.h"
#include "events/EventModels.h"
#include "tracks/AudioDecoder.h"

namespace cspot {
class StreamPlayer : public bell::Task {
 public:
  StreamPlayer(std::shared_ptr<cspot::EventLoop> eventLoop,
               std::unique_ptr<cspot::FileProvider> fileProvider,
               std::unique_ptr<cspot::AudioDecoder> audioDecoder);

  ~StreamPlayer() override;

 private:
  const char* LOG_TAG = "StreamPlayer";

  std::shared_ptr<cspot::EventLoop> eventLoop;
  std::shared_ptr<cspot::SpClient> spClient;
  std::shared_ptr<cspot::ApClient> apClient;
  std::unique_ptr<cspot::FileProvider> fileProvider;
  bell::Semaphore queueUpdateSemaphore;

  std::recursive_mutex playbackMutex;

  std::vector<SpotifyId> playbackQueue;
  std::unordered_map<SpotifyId, ProvidedFile> providedTracks;

  int currentTrackIndex = 0;
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
  // isPlaying (see the .cpp for why: a paused transfer still needs to
  // reach a real "ready" state, not stay stuck "buffering" forever). Safe
  // to call speculatively from handleFileProvided/handlePlayEvent/taskLoop
  // (playbackMutex is recursive, so nested calls from a caller already
  // holding it are fine).
  void maybeStartCurrentTrack();

  // isPlaying/isBuffering are sent to the server as-is (see
  // ConnectStateHandler's PLAYER_STATE_UPDATED handler) - callers must only
  // claim isPlaying=true once audio is actually flowing, not merely once a
  // CDN url/decrypt key are resolved. Confirmed against a real hardware
  // session: claiming isPlaying=true at file-ready time (before the decoder
  // was even opened) was read by the real Spotify app as the device never
  // successfully connecting - librespot-cpp's own PlayerEngine.cpp has the
  // exact same two-phase buffering->playing split, with a comment
  // documenting the same client-visible failure from getting this wrong.
  void announceState(bool isPlaying, bool isBuffering);
};
}  // namespace cspot
