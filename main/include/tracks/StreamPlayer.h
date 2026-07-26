#pragma once

#include <optional>

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

  // What's playing (or about to). TrackQueueHandler is the sole authority on
  // *which* track this is - StreamPlayer only ever gets told via
  // handleQueueUpdate()'s TrackQueueUpdate, never decides order itself.
  std::optional<SpotifyId> currentTrackId;
  std::optional<ProvidedFile> currentFile;

  // At most one track prefetched ahead - a cache/hint to avoid a redundant
  // fetch if the next official advance (natural EOF or a remote skip) lands
  // on exactly this track, never itself the authority on what's next.
  // Mirrors go-librespot's single secondaryStream (daemon/player.go), not a
  // preloaded window: fetching several tracks' worth of metadata/audio-key/
  // CDN at once was the real cause of a dealer WebSocket getting dropped
  // under the resulting network burst (real hardware logs, transfer path).
  std::optional<SpotifyId> nextTrackId;
  std::optional<ProvidedFile> nextFile;
  // Guards against calling fileProvider->provideTrack() more than once for
  // the same nextTrackId while its fetch is still in flight (nextFile stays
  // nullopt until FILE_PROVIDED lands - this flag is what distinguishes
  // "not requested yet" from "requested, waiting").
  bool nextFetchStarted = false;

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

  // Requests the single next-track prefetch (see nextTrackId/nextFile above)
  // once the current track is actually open - not at queue-update time, so
  // a transfer/skip never fetches more than one track's worth of metadata/
  // audio-key/CDN at once. Idempotent (nextFetchStarted guards re-requesting
  // the same track); a no-op if there's nothing to prefetch or it's already
  // in flight/resolved.
  void maybePrefetchNext();

  // Announces PlayerState.isPlaying/isPaused/isBuffering to the server
  // (see ConnectStateHandler's PLAYER_STATE_UPDATED handler).
  // isPlaying=false claims no session is loaded at all - callers must
  // only pass isBuffering=true (which implies isPlaying=false) once for
  // the very first track, not merely once a CDN url/decrypt key are
  // resolved. Confirmed against a real hardware session: claiming
  // isPlaying=false at file-ready time (before the decoder was even
  // opened) was read by the real Spotify app as the device never
  // successfully connecting.
  //
  // isPlaying itself is derived as !isBuffering, NOT from this class's
  // own isPlaying member (that member is the local decode gate, toggled
  // by pause/resume - reporting it directly as PlayerState.isPlaying
  // conflates "a session is loaded" with "audio is currently flowing",
  // which makes a paused device read as having nothing loaded in the
  // real app. isPaused is what reports the local decode gate instead;
  // this repo's own master branch found and documented this exact
  // failure mode ("is_playing means 'session active', NOT !is_paused").
  //
  // playbackId: only meaningful (and only ever passed) on the isBuffering=
  // false call, once audioDecoder->openStream() actually succeeded - a
  // fresh random id per track, never reused. Left empty on the earlier
  // isBuffering=true announce, since it isn't known yet; ConnectStateHandler
  // leaves the previous value untouched in that case rather than clearing
  // it (matches this repo's own PlayerEngine.cpp precedent: "playback_id
  // deliberately not touched here - not known until the stream actually
  // opens").
  void announceState(bool isBuffering, const std::string& playbackId = "");
};
}  // namespace cspot
