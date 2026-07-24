#include "tracks/StreamPlayer.h"
#include "FileProvider.h"
#include "Utils.h"

using namespace cspot;

namespace {
const size_t maxPreloadedTracks = 3;
}

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

  bool flushRequired = false;

  if (playbackQueue.empty()) {
    playbackQueue.push_back(*update.currentTrackId);
  }

  if (playbackQueue[0] != *update.currentTrackId) {
    flushRequired = true;
    playbackQueue[0] = *update.currentTrackId;
  }

  for (size_t nextTrackIdx = 0;
       nextTrackIdx < std::min(maxPreloadedTracks, update.nextTracks.size());
       nextTrackIdx++) {
    if (playbackQueue.size() <
        nextTrackIdx + 2) {  // +2 because current track is at index 0
      playbackQueue.push_back(update.nextTracks[nextTrackIdx]);
    }

    if (playbackQueue[nextTrackIdx + 1] != update.nextTracks[nextTrackIdx]) {
      if (currentTrackIndex >= static_cast<int>(nextTrackIdx + 1)) {
        // Only flush if we're ahead of the changed track
        flushRequired = true;
      }

      playbackQueue[nextTrackIdx + 1] = update.nextTracks[nextTrackIdx];
    }
  }

  if (flushRequired) {
    BELL_LOG(info, LOG_TAG, "Queue changed, flushing playback");
    handleFlushEvent();
  }

  // Request files for all tracks in the queue that we don't already have
  for (auto& trackId : playbackQueue) {
    if (!providedTracks.contains(trackId)) {
      providedTracks[trackId] = {};
      fileProvider->provideTrack(trackId);
    }
  }

  // Ensure we don't keep files for tracks that are no longer in the queue.
  // Collect stale ids first instead of erasing from providedTracks while
  // iterating it directly - erase() invalidates the range-for's iterator
  // mid-loop (a real hardware crash, reproduced: SpotifyId::operator==
  // reading garbage memory from the dangling iterator on the very next
  // comparison).
  std::vector<SpotifyId> staleTrackIds;
  for (auto& trackId : providedTracks) {
    if (std::find(playbackQueue.begin(), playbackQueue.end(), trackId.first) ==
        playbackQueue.end()) {
      staleTrackIds.push_back(trackId.first);
    }
  }
  for (auto& trackId : staleTrackIds) {
    providedTracks.erase(trackId);
    fileProvider->cancel(trackId);
  }
}

void StreamPlayer::handleFileProvided(const ProvidedFile& providedFile) {
  std::scoped_lock lock(playbackMutex);

  if (providedFile.isError) {
    BELL_LOG(error, LOG_TAG, "Error providing file for track {}",
             providedFile.itemId.uri);
    return;
  }

  if (this->providedTracks.contains(providedFile.itemId)) {
    this->providedTracks[providedFile.itemId] = providedFile;

    BELL_LOG(info, LOG_TAG, "Track {} is ready to play from file {}",
             providedFile.itemId.uri, providedFile.cdnUrl);

    if (providedFile.itemId == playbackQueue[0]) {
      // Still buffering here - the decoder hasn't been opened yet, let
      // alone produced any real audio. See announceState()'s doc comment.
      announceState(/*isPlaying=*/false, /*isBuffering=*/true);
    }
  } else {
    // Probably outdated request
  }

  maybeStartCurrentTrack();
  queueUpdateSemaphore.give();
}

void StreamPlayer::handlePlayEvent(bool shouldPlay) {
  std::scoped_lock lock(playbackMutex);
  BELL_LOG(info, LOG_TAG, "Received PLAYER_PLAY event, shouldPlay={}",
           shouldPlay);
  isPlaying = shouldPlay;
  if (shouldPlay) {
    maybeStartCurrentTrack();
  }
  queueUpdateSemaphore.give();
}

void StreamPlayer::handleFlushEvent() {
  std::scoped_lock lock(playbackMutex);
  flushRequested = true;
  queueUpdateSemaphore.give();
}

void StreamPlayer::maybeStartCurrentTrack() {
  std::scoped_lock lock(playbackMutex);
  if (!isPlaying || audioDecoder->isOpen() || !isCurrentTrackReady()) {
    return;
  }

  auto& file = providedTracks[playbackQueue[currentTrackIndex]];
  BELL_LOG(info, LOG_TAG, "Opening CDN stream for {}: {}", file.itemId.uri,
           file.cdnUrl);
  auto res = audioDecoder->openStream(file.cdnUrl, file.decryptionKey,
                                      file.itemId);
  BELL_LOG(info, LOG_TAG, "openStream() returned for {}", file.itemId.uri);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to open CDN stream: {}", res.error());
    return;
  }

  if (file.itemId == playbackQueue[0]) {
    // Real playback is genuinely starting now - the earlier
    // handleFileProvided() announce only ever meant "buffering".
    announceState(/*isPlaying=*/true, /*isBuffering=*/false);
  }
}

void StreamPlayer::announceState(bool isPlaying, bool isBuffering) {
  std::scoped_lock lock(playbackMutex);

  PlayerStateUpdate stateUpdate{
      .isPlaying = isPlaying,
      .isBuffering = isBuffering,
      .timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count(),
      .positionAsOfTimestamp = 0,
      .playbackDurationMs = 0,
  };

  if (providedTracks[playbackQueue[0]].trackMetadata) {
    stateUpdate.playbackDurationMs =
        providedTracks[playbackQueue[0]].trackMetadata->durationMs;
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
      currentTrackIndex = 0;
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
      currentTrackIndex++;
      maybeStartCurrentTrack();
    }
  } else {
    queueUpdateSemaphore.take(100);
  }
}

bool StreamPlayer::isCurrentTrackReady() {
  if (playbackQueue.empty() ||
      currentTrackIndex >= static_cast<int>(playbackQueue.size())) {
    return false;
  }

  auto& currentTrackId = playbackQueue[currentTrackIndex];
  if (!providedTracks.contains(currentTrackId)) {
    return false;
  }

  auto& providedFile = providedTracks[currentTrackId];
  return !providedFile.cdnUrl.empty() && !providedFile.decryptionKey.empty() &&
         providedFile.trackMetadata.has_value();
}
