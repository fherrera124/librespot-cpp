#include "tracks/StreamPlayer.h"
#include <algorithm>
#include <random>
#include "FileProvider.h"
#include "Utils.h"

using namespace cspot;

namespace {
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

StreamPlayer::StreamPlayer(
    std::shared_ptr<cspot::EventLoop> eventLoop,
    std::unique_ptr<cspot::FileProvider> fileProvider,
    std::unique_ptr<cspot::AudioDecoder> audioDecoder,
    PlayerStateAnnounceCallback playerStateAnnounceCallback)
    // taskLoop() calls directly into TLS handshake (HTTPS CDN fetch), AES
    // decrypt, and Vorbis decode on this stack - this codebase's own git
    // history already has two hardware stack-overflow crashes from
    // undersized task stacks doing similar HTTPS/crypto work.
    : bell::Task("cspot_player", 32 * 1024),
      eventLoop(std::move(eventLoop)),
      fileProvider(std::move(fileProvider)),
      playerStateAnnounceCallback(std::move(playerStateAnnounceCallback)),
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

  bool trackChanged =
      !currentTrackId || *currentTrackId != *update.currentTrackId;

  if (!trackChanged) {
    return;
  }

  // No prefetch/"next" tracking - minimum needed to play the current
  // track, nothing ahead of it. Cancels FileProvider's work on whatever's
  // being discarded, then requests the new current track.
  if (currentTrackId && !currentFile) {
    fileProvider->cancel(*currentTrackId);
  }
  currentTrackId = update.currentTrackId;
  currentFile.reset();
  fileProvider->provideTrack(*currentTrackId);

  // maybeStartCurrentTrack() is deliberately NOT called here, unlike
  // handleFileProvided()/handlePlayEvent() - only taskLoop() may open a
  // stream, and only after it has processed any pending flush in that
  // same pass. Opening one here first would race taskLoop() into tearing
  // it down as stale on its next flush check.
  BELL_LOG(info, LOG_TAG, "Queue changed, flushing playback");
  handleFlushEvent();
}

void StreamPlayer::handleFileProvided(const ProvidedFile& providedFile) {
  std::scoped_lock lock(playbackMutex);

  if (providedFile.isError) {
    BELL_LOG(error, LOG_TAG, "Error providing file for track {}",
             providedFile.itemId.uri);
    if (currentTrackId && providedFile.itemId == *currentTrackId &&
        !currentFile) {
      // Same treatment as natural EOF just below (see that branch's own
      // comment) - a track that can never load needs to give up exactly
      // the same way as one that finished normally, not just log locally.
      // Without this, a track whose audio key request failed (denied by
      // the AP, or orphaned by a reconnect - see ApClient::
      // connectAndAuthenticate()'s own comment) leaves the Spotify client
      // waiting forever for a PlayerState update that never comes - shown
      // client-side as "Spotify can't play this right now" after its own
      // timeout.
      currentTrackId.reset();
      eventLoop->post(EventLoop::EventType::TRACK_ENDED, std::monostate{});
    }
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

  announceState(/*isBuffering=*/false, generatePlaybackId());
}

void StreamPlayer::announceState(bool isBuffering,
                                 std::optional<std::string> playbackId) {
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

  playerStateAnnounceCallback(stateUpdate);
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
      // that we ran out of audio, and the resulting QUEUE_UPDATED is what
      // actually advances playback (handleQueueUpdate() requests the new
      // current track's file from scratch, no prefetch to promote).
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
