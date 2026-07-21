#pragma once

#include <stddef.h>  // for size_t
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "TrackReference.h"

#include "protobuf/metadata.pb.h"  // for Track, _Track, AudioFile, Episode

namespace bell {
class WrappedSemaphore;
};

namespace cspot {
struct Context;
class AccessKeyFetcher;
class CDNAudioFile;
struct CDNConnection;
class TrackLoader;

// Used in got track info event
struct TrackInfo {
  std::string name, album, artist, imageUrl, trackId;
  uint32_t duration, number, discNumber;

  void loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid);
  void loadPbEpisode(Episode* pbEpisode, const std::vector<uint8_t>& gid);
};

class QueuedTrack {
 public:
  QueuedTrack(TrackReference& ref, std::shared_ptr<cspot::Context> ctx,
              uint32_t requestedPosition = 0);
  ~QueuedTrack();

  enum class State {
    QUEUED,
    PENDING_META,
    KEY_REQUIRED,
    PENDING_KEY,
    CDN_REQUIRED,
    READY,
    FAILED
  };

  std::shared_ptr<bell::WrappedSemaphore> loadedSemaphore;

  // Read from TrackLoader's own task (processTrack()'s step functions) and
  // TrackPlayer's task (runTask()) - genuinely cross-thread, hence atomic.
  // Private: every transition goes through setState() instead of letting
  // any call site assign the field directly, so there's one place that
  // can enforce "once failed, stay failed" - see setState()'s own comment.
  // This atomic publish (write the other fields below, then setState() as
  // the last step) / observe (getState(), then read the fields the
  // observed state promises are valid) pattern is the *only*
  // synchronization this class's other fields get - there is no mutex
  // guarding them. Every step function was audited to confirm no field
  // write happens after its transition's setState() call, and every
  // reader was audited to confirm it never touches a field without having
  // just observed the corresponding transition via getState() first (see
  // the removal of stepLoadMetadata()/stepLoadAudioFile()'s trackListMutex
  // parameter). Removing this discipline requires re-deriving that
  // argument from scratch, not just reaching for a mutex out of caution.
  State getState() const { return state.load(); }
  void setState(State newState);

  TrackReference ref;           // Holds GID, URI and Context
  TrackInfo trackInfo;  // Full track information fetched from spotify, name etc

  uint32_t requestedPosition;
  std::string identifier;

  // Format of the file actually selected in stepParseMetadata() (not
  // necessarily ctx->config.audioFormat - see the fallback chain there).
  // TrackPlayer reads this after getAudioFile() to pick a decoder
  // (Vorbis vs MP3). Default is irrelevant - always overwritten whenever
  // fileId gets set. See docs/spotify_component_analysis.md, finding F60.
  AudioFormat selectedFormat = AudioFormat_OGG_VORBIS_160;

  // Will return nullptr if the track is not ready. `connection` is
  // TrackPlayer's CDN connection, kept alive across tracks - see
  // CDNConnection's own comment (CDNAudioFile.h).
  std::shared_ptr<cspot::CDNAudioFile> getAudioFile(CDNConnection& connection);

  // --- Steps ---
  void stepLoadMetadata(
      Track* pbTrack, Episode* pbEpisode,
      std::shared_ptr<bell::WrappedSemaphore> updateSemaphore);

  void stepParseMetadata(Track* pbTrack, Episode* pbEpisode);

  void stepLoadAudioFile(
      std::shared_ptr<bell::WrappedSemaphore> updateSemaphore);

  void stepLoadCDNUrl(const std::string& accessKey);

  void expire();

 private:
  std::atomic<State> state{State::QUEUED};

  std::shared_ptr<cspot::Context> ctx;

  uint64_t pendingMercuryRequest = 0;
  uint32_t pendingAudioKeyRequest = 0;

  std::vector<uint8_t> trackId, fileId, audioKey;
  std::string cdnUrl;

  // Bounded retry budget for whichever preload step (metadata/audio-key/
  // CDN-url) is currently in flight - reset to 0 whenever a step succeeds
  // and the state machine advances to the next one, so each step gets its
  // own fresh budget. See the steps' own .cpp comments.
  int loadAttempts = 0;
  std::chrono::steady_clock::time_point retryNotBeforeTime{};
};

class TrackQueue {
 public:
  // accessKeyFetcher: injected rather than constructed internally, so a
  // test can supply a fake instead of the real one's blocking HTTPS POST
  // to accounts.spotify.com (see cspot/tests/f104_queuedtrack_state_race_
  // test.cpp's own comment on why it couldn't drive TrackLoader::runTask()
  // directly for exactly this reason). Forwarded straight through to
  // trackLoader's constructor - see TrackLoader.h.
  TrackQueue(std::shared_ptr<cspot::Context> ctx,
             std::shared_ptr<cspot::AccessKeyFetcher> accessKeyFetcher);
  ~TrackQueue();

  enum class SkipDirection { NEXT, PREV };

  std::shared_ptr<bell::WrappedSemaphore> playableSemaphore;
  std::atomic<bool> notifyPending = false;

  // Forwards to trackLoader's own stopTask() - TrackQueue itself no
  // longer runs a background task (that's TrackLoader's job now), but
  // keeps this method so callers (PlaybackController.cpp) don't need to
  // know about the split.
  void stopTask();

  bool hasTracks();
  bool isFinished();
  // Repeat-context: repeats the whole queue, looping back to the first
  // track when it ends (restartFromBeginning()). Not per-track repeat -
  // classic SPIRC only exposes one repeat flag to the client, and it
  // means "repeat context" there, same as librespot's modern
  // Connect-state repeating_context (as opposed to its separate
  // repeating_track, which classic SPIRC has no way to express). See
  // docs/spotify_component_analysis.md, findings F88/F92.
  void setRepeatContext(bool repeat);
  bool isRepeatingContext();
  void restartFromBeginning();
  // currentPositionMs: caller-tracked playback position, used only to
  // decide PREV's "restart current track vs. go to the actual previous
  // one" (<3s in) - see the comment on the definition. Was read from
  // PlaybackState's SPIRC Frame before this class stopped depending on
  // it (docs/dealer_websocket_migration.md, Fase 6 "corte completo").
  // allowSeeking: from the dealer command's own options.allow_seeking
  // (PREV only) - false means always go to the real previous track,
  // ignoring currentPositionMs entirely. Matches go-librespot's
  // skipPrev(ctx, allowSeeking) - it isn't this class's own policy call,
  // the client requesting the skip decides it per-command.
  bool skipTrack(SkipDirection dir, uint32_t currentPositionMs,
                 bool expectNotify = true, bool allowSeeking = true);
  // tracks/startIndex: the list to load and which entry to start at -
  // used to come from PlaybackState's remoteTracks/playing_track_index
  // (populated by SpircHandler decoding a SPIRC Load/Replace frame);
  // now an explicit parameter so this class has no protocol-specific
  // dependency - both SPIRC and connect-state's Transfer command just
  // resolve their own track list and pass it in the same way.
  bool updateTracks(const std::vector<TrackReference>& tracks, int startIndex,
                    uint32_t requestedPosition = 0, bool initial = false);
  // Connect-state queue editing (docs/dealer_websocket_migration.md §11) -
  // both edit the tracks AROUND the currently-playing one without touching
  // it (no reload, no audible restart), unlike updateTracks().
  //
  // insertNext: "add to queue" - the track plays after the current one and
  // after any earlier still-pending insertNext() tracks (FIFO, like the
  // real apps' queue), before the context continues.
  // replaceUpcoming: "set queue" - currentTracks becomes
  // prevTracks + [current] + nextTracks, with the index at [current].
  // @returns false only when nothing is playing (no current track to
  // anchor the edit to).
  bool insertNext(const TrackReference& track);
  bool replaceUpcoming(const std::vector<TrackReference>& prevTracks,
                       const std::vector<TrackReference>& nextTracks);
  // Queue display ("playing next"/"previously played") for connect-state's
  // PlayerState.prev_tracks/next_tracks - mirrors go-librespot's
  // tracks.List.PrevTracks()/NextTracks() (tracks/tracks.go), just against
  // this project's own currentTracks/currentTracksIndex instead of a
  // lazy-fetched context iterator. maxCount is the caller's own encode-time
  // budget (PlayerEngine.cpp), not tied to MAX_TRACKS_PRELOAD (which
  // bounds audio buffering, a different concern).
  std::vector<TrackReference> getPrevTracks(size_t maxCount);
  std::vector<TrackReference> getNextTracks(size_t maxCount);
  TrackInfo getTrackInfo(std::string_view identifier);
  std::shared_ptr<QueuedTrack> consumeTrack(
      std::shared_ptr<QueuedTrack> prevSong, int& offset);

 private:
  static const int MAX_TRACKS_PRELOAD = 3;

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<bell::WrappedSemaphore> processSemaphore;

  std::deque<std::shared_ptr<QueuedTrack>> preloadedTracks;
  std::vector<TrackReference> currentTracks;
  std::mutex tracksMutex;

  int16_t currentTracksIndex = -1;

  // How many insertNext() tracks are still ahead of the play position -
  // makes consecutive "add to queue"s land in FIFO order (each new insert
  // goes AFTER the pending ones, not right at index+1 which would play
  // them backwards). Decremented as playback advances past them
  // (skipTrack(NEXT)), reset whenever the whole list is replaced.
  int16_t pendingQueuedCount = 0;

  bool shouldRepeatContext = false;

  bool queueNextTrack(int offset = 0, uint32_t positionMs = 0);

  // Callback targets for trackLoader (see TrackLoader.h's SnapshotFn/
  // TopUpFn) - a near-verbatim extraction of what runTask()'s copy-loop
  // and processTrack()'s CDN_REQUIRED top-up did before the split, just
  // named methods TrackLoader calls through a callback instead of being
  // inlined in the same class that owns preloadedTracks/tracksMutex.
  // Private: not part of TrackQueue's own public API, only ever invoked
  // via the lambdas handed to trackLoader's constructor.
  std::deque<std::shared_ptr<QueuedTrack>> snapshotPreloadedTracks();
  void tryTopUpLookahead();

  // Constructed last, after every member its background task's callbacks
  // touch (tracksMutex/preloadedTracks/currentTracks/etc. above) - so it's
  // destroyed FIRST, stopping that task before the state it calls back
  // into goes away. A prior constructor-init-list ordering mistake in
  // this project shipped a real -Wreorder build break (commit 2f49294) -
  // don't reorder this member without re-checking that history.
  std::unique_ptr<TrackLoader> trackLoader;
};
}  // namespace cspot
