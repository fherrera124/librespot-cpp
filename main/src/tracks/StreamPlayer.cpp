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

// Flattens a resolved Track proto into the outward TrackChanged
// notification's display fields: first artist only, album name, cover art
// URL from the album's first cover image. Episodes aren't handled here -
// FileProvider doesn't fetch episode metadata yet (its own TODO), so
// currentFile->trackMetadata is Track-only in practice.
cspot::TrackMetadata toTrackMetadata(const cspot::SpotifyId& trackId,
                                     const cspot_proto::Track& track) {
  cspot::TrackMetadata metadata;
  metadata.uri = trackId.uri;
  metadata.name = track.name;
  if (!track.artists.empty()) {
    metadata.artist = track.artists[0].name;
  }
  if (track.album.hasValue) {
    metadata.album = track.album.value.name;
    auto& coverGroup = track.album.value.coverGroup;
    if (coverGroup.hasValue && !coverGroup.value.images.empty()) {
      static const char* hexDigits = "0123456789abcdef";
      std::string hex;
      auto& fileId = coverGroup.value.images[0].fileId;
      hex.reserve(fileId.size() * 2);
      for (std::byte b : fileId) {
        auto v = std::to_integer<uint8_t>(b);
        hex += hexDigits[v >> 4];
        hex += hexDigits[v & 0x0f];
      }
      metadata.imageUrl = "https://i.scdn.co/image/" + hex;
    }
  }
  metadata.durationMs = static_cast<uint32_t>(track.durationMs);
  return metadata;
}
}  // namespace

StreamPlayer::StreamPlayer(
    std::shared_ptr<cspot::EventLoop> eventLoop,
    std::unique_ptr<cspot::FileProvider> fileProvider,
    std::unique_ptr<cspot::AudioDecoder> audioDecoder,
    PlayerStateAnnounceCallback playerStateAnnounceCallback,
    std::shared_ptr<cspot::AudioSink> audioSink)
    : bell::Task("cspot_player", 32 * 1024),
      eventLoop(std::move(eventLoop)),
      fileProvider(std::move(fileProvider)),
      playerStateAnnounceCallback(std::move(playerStateAnnounceCallback)),
      audioSink(std::move(audioSink)),
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

  eventLoop->registerHandler(
      EventLoop::EventType::PLAYER_SEEK, [&](EventLoop::Event&& ev) {
        auto event = std::move(ev);
        handleSeekEvent(std::get<int64_t>(event.payload));
      });
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
      // A separate event from the natural-EOF one just below - TRACK_UNPLAYABLE
      // always advances regardless of repeat-track, unlike TRACK_ENDED (no
      // audio to repeat). Without this, a track whose audio key request
      // failed leaves the Spotify client waiting forever for a PlayerState
      // update - shown client-side as "Spotify can't play this right now".
      currentTrackId.reset();
      eventLoop->post(EventLoop::EventType::TRACK_UNPLAYABLE,
                      std::monostate{});
    }
    return;
  }

  if (currentTrackId && providedFile.itemId == *currentTrackId &&
      !currentFile) {
    currentFile = providedFile;

    BELL_LOG(info, LOG_TAG, "Track {} is ready to play",
             providedFile.itemId.uri);

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

void StreamPlayer::handleSeekEvent(int64_t positionMs) {
  std::scoped_lock lock(playbackMutex);
  // Deferred to taskLoop() instead of calling audioDecoder->seekToMs()
  // here: taskLoop() calls processPacket() without holding playbackMutex
  // (see its own comment), so a direct call from this thread would race
  // it on the same decoder/CDNDataStream, which has no locking of its own.
  pendingSeekMs = positionMs;
  queueUpdateSemaphore.give();
}

void StreamPlayer::maybeStartCurrentTrack() {
  std::scoped_lock lock(playbackMutex);
  if (audioDecoder->isOpen() || !isCurrentTrackReady()) {
    return;
  }

  // Deliberately NOT gated on isPlaying: a paused transfer still needs the
  // track opened and ready, same as a playing one - only actually feeding
  // decoded audio (taskLoop()'s own isPlaying check before processPacket())
  // should wait. Gating the open itself on isPlaying leaves isBuffering
  // stuck at true for any transfer that starts paused.
  auto& file = *currentFile;
  BELL_LOG(debug, LOG_TAG, "Opening CDN stream for {}: {}", file.itemId.uri,
           file.cdnUrl);
  auto res = audioDecoder->openStream(file.cdnUrl, file.decryptionKey,
                                      file.itemId, file.format);
  BELL_LOG(info, LOG_TAG, "openStream() returned for {}", file.itemId.uri);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to open CDN stream: {}", res.error());
    return;
  }

  announceState(/*isBuffering=*/false, generatePlaybackId(), /*positionMs=*/0);
}

void StreamPlayer::announceState(bool isBuffering,
                                 std::optional<std::string> playbackId,
                                 int64_t positionMs) {
  std::scoped_lock lock(playbackMutex);

  PlayerStateUpdate stateUpdate{
      // See this method's own doc comment for why isPlaying stays true
      // here regardless of the local isPlaying member.
      .isPlaying = true,
      .isPaused = !isPlaying,
      .isBuffering = isBuffering,
      .timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count(),
      .positionAsOfTimestamp = positionMs,
      .playbackDurationMs = 0,
      .playbackId = playbackId,
  };

  if (currentFile && currentFile->trackMetadata) {
    stateUpdate.playbackDurationMs = currentFile->trackMetadata->durationMs;
    // Only once the track is actually ready to play (matches playbackId's
    // own timing above) - the isBuffering=true announce fires before the
    // metadata fetch in FileProvider even starts.
    if (!isBuffering && currentTrackId) {
      stateUpdate.trackMetadata =
          toTrackMetadata(*currentTrackId, *currentFile->trackMetadata);
    }
  }

  playerStateAnnounceCallback(stateUpdate);
}

void StreamPlayer::taskLoop() {
  {
    std::scoped_lock lock(playbackMutex);
    if (flushRequested) {
      BELL_LOG(debug, LOG_TAG, "Flush requested, resetting state");
      flushRequested = false;
      audioDecoder->resetStream();
      if (suppressNextSinkFlush) {
        suppressNextSinkFlush = false;  // natural end - see its own comment
      } else {
        // Sink may still hold audio from the abandoned track/position
        // (same reasoning as the seek flush below).
        audioSink->flush();
      }
    }

    // Applied here (not in handleSeekEvent()) so seekToMs() only runs on
    // this task's own thread, matching processPacket() below - see
    // handleSeekEvent()'s comment.
    if (pendingSeekMs) {
      int64_t positionMs = *pendingSeekMs;
      pendingSeekMs.reset();

      if (!audioDecoder->isOpen()) {
        BELL_LOG(warn, LOG_TAG, "Ignoring seek to {}ms - no track open",
                 positionMs);
      } else {
        auto res = audioDecoder->seekToMs(positionMs);
        if (!res) {
          BELL_LOG(error, LOG_TAG, "Seek to {}ms failed: {}", positionMs,
                   res.error());
        } else {
          // Sink may still hold audio decoded from the old position -
          // without this, a seek briefly plays stale audio first.
          audioSink->flush();
        }
      }
    }

    maybeStartCurrentTrack();
  }

  if (isPlaying && audioDecoder->isOpen()) {
    // Outside playbackMutex: this can block on network/I2S, and holding
    // the lock would stall handleFlushEvent/handleQueueUpdate/
    // handlePlayEvent (EventLoop's own dispatch task).
    audioDecoder->processPacket();

    std::scoped_lock lock(playbackMutex);
    if (audioDecoder->isEOF()) {
      BELL_LOG(info, LOG_TAG, "Track ended");
      audioDecoder->resetStream();
      // Also drop currentFile/currentTrackId (not just the decoder):
      // maybeStartCurrentTrack() runs again before QUEUE_UPDATED can
      // arrive, and would otherwise see the just-ended track as still
      // ready and reopen it for ~1s.
      currentFile.reset();
      currentTrackId.reset();
      suppressNextSinkFlush = true;  // see its own comment (StreamPlayer.h)
      // TrackQueueHandler is the sole authority on what's next (same as
      // skip_next) - this just signals we ran out of audio; the
      // resulting QUEUE_UPDATED is what actually advances playback.
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
