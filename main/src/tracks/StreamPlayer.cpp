#include "tracks/StreamPlayer.h"
#include <algorithm>
#include <random>
#include "FileProvider.h"
#include "Utils.h"

using namespace cspot;

namespace {
// Fresh per track start, hex-encoded (not base64, unlike session_id - a
// real divergence from go-librespot found and documented in this repo's
// own PlayerEngine.cpp). Same generator idiom as ConnectStateHandler's own
// generateSessionId().
std::string generatePlaybackId() {
  static std::independent_bits_engine<std::default_random_engine, CHAR_BIT,
                                      unsigned char>
      randomEngine{std::default_random_engine(std::random_device{}())};
  static const char* hexDigits = "0123456789abcdef";
  std::string id;
  id.reserve(32);
  std::generate_n(std::back_inserter(id), 32,
                  [] { return hexDigits[randomEngine() % 16]; });
  return id;
}
}  // namespace

StreamPlayer::StreamPlayer(std::shared_ptr<cspot::EventLoop> eventLoop,
                           std::unique_ptr<cspot::FileProvider> fileProvider,
                           std::unique_ptr<cspot::AudioDecoder> audioDecoder)
    // taskLoop() calls directly into TLS handshake (HTTPS CDN fetch), AES
    // decrypt, and Vorbis decode on this stack - this codebase's own git
    // history already has two hardware stack-overflow crashes from
    // undersized task stacks doing similar HTTPS/crypto work.
    : bell::Task("cspot_player", 32 * 1024),
      eventLoop(std::move(eventLoop)),
      fileProvider(std::move(fileProvider)),
      audioDecoder(std::move(audioDecoder)) {
  registerHandlers();
  startTask();
}

StreamPlayer::~StreamPlayer() {
  stopTask();
}

void StreamPlayer::registerHandlers() {
  eventLoop->registerHandler(
      EventLoop::EventType::QUEUE_UPDATED, [&](EventLoop::Event&& ev) {
        BELL_LOG(info, LOG_TAG, "Received QUEUE_UPDATED event");
        auto event = std::move(ev);
        auto& queueUpdate = std::get<TrackQueueUpdate>(event.payload);
        handleQueueUpdate(queueUpdate);
      });

  eventLoop->registerHandler(
      EventLoop::EventType::FILE_PROVIDED, [&](EventLoop::Event&& ev) {
        auto event = std::move(ev);
        auto& providedFile = std::get<ProvidedFile>(event.payload);
        handleFileProvided(providedFile);
      });

  eventLoop->registerHandler(EventLoop::EventType::PLAYER_PLAY,
                             [&](EventLoop::Event&& ev) {
                               auto event = std::move(ev);
                               handlePlayEvent(std::get<bool>(event.payload));
                             });

  eventLoop->registerHandler(EventLoop::EventType::PLAYER_FLUSH,
                             [&](auto&& /*ev*/) { handleFlushEvent(); });
}

void StreamPlayer::handleQueueUpdate(const TrackQueueUpdate& update) {
  std::scoped_lock lock(playbackMutex);

  if (!update.currentTrackId) {
    BELL_LOG(warn, LOG_TAG, "Received queue update without current track id");
    return;
  }

  std::optional<SpotifyId> newNextTrackId =
      update.nextTracks.empty() ? std::nullopt
                                : std::make_optional(update.nextTracks[0]);

  bool trackChanged =
      !currentTrackId || *currentTrackId != *update.currentTrackId;

  if (trackChanged) {
    if (nextTrackId && *nextTrackId == *update.currentTrackId) {
      // Prefetch hit: what we already fetched (or are still fetching) for
      // "next" is exactly the new current track - promote it instead of
      // asking FileProvider for it again. Mirrors go-librespot's
      // secondaryStream -> primaryStream swap (daemon/controls.go).
      currentTrackId = *nextTrackId;
      currentFile = nextFile;
    } else {
      // Not a hit - stop FileProvider's work on whatever's being discarded.
      if (currentTrackId && !currentFile) {
        fileProvider->cancel(*currentTrackId);
      }
      if (nextTrackId && !nextFile) {
        fileProvider->cancel(*nextTrackId);
      }
      currentTrackId = update.currentTrackId;
      currentFile.reset();
      fileProvider->provideTrack(*currentTrackId);
    }

    nextTrackId = newNextTrackId;
    nextFile.reset();
    nextFetchStarted = false;

    // Only requests the reset here - maybeStartCurrentTrack() is
    // deliberately NOT called from this function (unlike
    // handleFileProvided/handlePlayEvent, which do call it, so the decoder
    // is opened from whichever thread gets there first, serialized by
    // playbackMutex - not exclusively taskLoop()'s thread). Calling it here
    // could open the new track's stream before taskLoop() gets a chance to
    // process this same flush and tear it back down as if it were the
    // stale one.
    BELL_LOG(info, LOG_TAG, "Queue changed, flushing playback");
    handleFlushEvent();
  } else if (newNextTrackId != nextTrackId) {
    // Same current track, but the upcoming one shifted (e.g. a set_queue
    // command) - drop an in-flight/resolved prefetch for the old "next",
    // it's no longer wanted.
    if (nextTrackId && !nextFile) {
      fileProvider->cancel(*nextTrackId);
    }
    nextTrackId = newNextTrackId;
    nextFile.reset();
    nextFetchStarted = false;
  }

  // Safe to call unconditionally: only touches FileProvider bookkeeping and
  // reads audioDecoder->isOpen(), never mutates the decoder itself.
  maybePrefetchNext();
}

void StreamPlayer::handleFileProvided(const ProvidedFile& providedFile) {
  std::scoped_lock lock(playbackMutex);

  if (providedFile.isError) {
    BELL_LOG(error, LOG_TAG, "Error providing file for track {}",
             providedFile.itemId.uri);
    return;
  }

  if (currentTrackId && providedFile.itemId == *currentTrackId &&
      !currentFile) {
    currentFile = providedFile;

    BELL_LOG(info, LOG_TAG, "Track {} is ready to play from file {}",
             providedFile.itemId.uri, providedFile.cdnUrl);

    // Still buffering here - the decoder hasn't been opened yet, let alone
    // produced any real audio. See announceState()'s doc comment.
    announceState(/*isBuffering=*/true);
  } else if (nextTrackId && providedFile.itemId == *nextTrackId && !nextFile) {
    nextFile = providedFile;
    BELL_LOG(info, LOG_TAG, "Track {} prefetched, ready for next advance",
             providedFile.itemId.uri);
  } else {
    // Stale/cancelled request (superseded before it resolved) - ignore.
  }

  maybeStartCurrentTrack();
  queueUpdateSemaphore.give();
}

void StreamPlayer::handlePlayEvent(bool shouldPlay) {
  std::scoped_lock lock(playbackMutex);
  BELL_LOG(info, LOG_TAG, "Received PLAYER_PLAY event, shouldPlay={}",
           shouldPlay);
  isPlaying = shouldPlay;
  // Not gated on shouldPlay: if the track was already opened while paused,
  // this is what re-announces the corrected isPlaying value on resume
  // (maybeStartCurrentTrack() is a no-op if the decoder's already open,
  // beyond that one announce).
  maybeStartCurrentTrack();
  queueUpdateSemaphore.give();
}

void StreamPlayer::handleFlushEvent() {
  std::scoped_lock lock(playbackMutex);
  flushRequested = true;
  queueUpdateSemaphore.give();
}

void StreamPlayer::maybeStartCurrentTrack() {
  std::scoped_lock lock(playbackMutex);
  if (audioDecoder->isOpen() || !isCurrentTrackReady()) {
    return;
  }

  // Deliberately NOT gated on isPlaying: a paused transfer still needs the
  // track opened and ready, exactly like a playing one - only actually
  // feeding decoded audio (taskLoop()'s own separate isPlaying check
  // before calling processPacket()) should wait on isPlaying. Confirmed
  // against a real Spotify session: gating the open itself on isPlaying
  // left isBuffering stuck at true forever for any transfer that started
  // paused (the only place that ever corrected it to false required
  // isPlaying=true first) - the real app read that as the device being
  // permanently stuck loading and greyed out its Play button. Mirrors
  // librespot-cpp's TrackPlayer, which always loads a track regardless of
  // its own startPaused flag.
  auto& file = *currentFile;
  BELL_LOG(info, LOG_TAG, "Opening CDN stream for {}: {}", file.itemId.uri,
           file.cdnUrl);
  auto res = audioDecoder->openStream(file.cdnUrl, file.decryptionKey,
                                      file.itemId);
  BELL_LOG(info, LOG_TAG, "openStream() returned for {}", file.itemId.uri);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to open CDN stream: {}", res.error());
    return;
  }

  // Ready now - a session is loaded regardless of the local play/pause
  // state, since "ready but paused" is a real, valid state distinct from
  // "still buffering". See announceState()'s doc comment.
  announceState(/*isBuffering=*/false, generatePlaybackId());

  // Only now that the current track is confirmed open do we go fetch the
  // next one - keeps a transfer/skip from ever requesting more than one
  // track's worth of metadata/audio-key/CDN work at once (a burst of that
  // was reproduced on real hardware killing the dealer WebSocket).
  maybePrefetchNext();
}

void StreamPlayer::maybePrefetchNext() {
  std::scoped_lock lock(playbackMutex);
  // flushRequested closes this same race handleQueueUpdate() otherwise
  // opens: it sets the flag and calls this function in the same breath,
  // but the actual audioDecoder->resetStream() only happens later, on
  // taskLoop()'s own thread - until then, isOpen() can still be reporting
  // the *previous* (about-to-be-torn-down) track as open, which let this
  // fire a prefetch for the new "next" track before the newly-transferred-
  // to "current" track had even started loading. Reproduced on real
  // hardware: two CDN streams' worth of network traffic firing at once
  // right at transfer time - the exact burst this function's other call
  // site (maybeStartCurrentTrack()) was already written to prevent.
  if (!nextTrackId || nextFetchStarted || flushRequested ||
      !audioDecoder->isOpen()) {
    return;
  }
  nextFetchStarted = true;
  fileProvider->provideTrack(*nextTrackId);
}

void StreamPlayer::announceState(bool isBuffering,
                                 const std::string& playbackId) {
  std::scoped_lock lock(playbackMutex);

  PlayerStateUpdate stateUpdate{
      // Both of announceState()'s callers only fire once a track is
      // known/loading (handleFileProvided's isBuffering=true announce,
      // maybeStartCurrentTrack()'s isBuffering=false one) - isPlaying
      // means "a session is loaded", never "audio already flowing", so it
      // stays true through buffering too. Tying it to !isBuffering here
      // reported the device as having nothing loaded for the whole
      // buffering window on the real desktop client - see
      // ConnectStateHandler::handleTransferCommand()'s own comment on the
      // same fix.
      .isPlaying = true,
      .isPaused = !isPlaying,
      .isBuffering = isBuffering,
      .timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count(),
      .positionAsOfTimestamp = 0,
      .playbackDurationMs = 0,
      .playbackId = playbackId,
  };

  if (currentFile && currentFile->trackMetadata) {
    stateUpdate.playbackDurationMs = currentFile->trackMetadata->durationMs;
  }

  eventLoop->post(EventLoop::EventType::PLAYER_STATE_UPDATED, stateUpdate);
}

void StreamPlayer::taskLoop() {
  {
    std::scoped_lock lock(playbackMutex);
    if (flushRequested) {
      BELL_LOG(info, LOG_TAG, "Flush requested, resetting state");
      flushRequested = false;
      audioDecoder->resetStream();
    }
    maybeStartCurrentTrack();
  }

  if (isPlaying && audioDecoder->isOpen()) {
    // Deliberately outside playbackMutex: this can block for an HTTP
    // round-trip or on the I2S sink's ring buffer, and holding the lock
    // here would stall handleFlushEvent/handleQueueUpdate/handlePlayEvent,
    // which run on the EventLoop's own dispatch task, not this one.
    audioDecoder->processPacket();

    std::scoped_lock lock(playbackMutex);
    if (audioDecoder->isEOF()) {
      BELL_LOG(info, LOG_TAG, "Track ended, moving to next track");
      audioDecoder->resetStream();
      // Also drop currentFile/currentTrackId here, not just the decoder:
      // the next taskLoop() iteration's maybeStartCurrentTrack() (top of
      // this function) runs before QUEUE_UPDATED can possibly arrive back
      // from ConnectStateHandler's advanceToNextTrack(), and without this
      // it still sees isCurrentTrackReady()==true for the track that just
      // ended - reopening it from scratch for ~1s before the real next
      // track's QUEUE_UPDATE lands and flushes it back out again
      // (reproduced on real hardware: a spurious re-open/immediate-flush
      // of the just-finished track on every natural advance).
      currentFile.reset();
      currentTrackId.reset();
      // TrackQueueHandler (via ConnectStateHandler) is the sole authority
      // on what's next, same as a remote skip_next - this just signals
      // that we ran out of audio, and the resulting QUEUE_UPDATED (which
      // may hit the nextTrackId/nextFile prefetch above) is what actually
      // advances playback.
      eventLoop->post(EventLoop::EventType::TRACK_ENDED, std::monostate{});
    }
  } else {
    queueUpdateSemaphore.take(100);
  }
}

bool StreamPlayer::isCurrentTrackReady() {
  return currentFile.has_value() && !currentFile->cdnUrl.empty() &&
         !currentFile->decryptionKey.empty() &&
         currentFile->trackMetadata.has_value();
}
