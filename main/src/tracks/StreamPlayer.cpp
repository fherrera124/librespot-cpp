#include "tracks/StreamPlayer.h"
#include "FileProvider.h"
#include "Utils.h"
#include "bell/utils/Utils.h"

using namespace cspot;

namespace {
const size_t maxPreloadedTracks = 3;
}

StreamPlayer::StreamPlayer(std::shared_ptr<cspot::EventLoop> eventLoop,
                           std::unique_ptr<cspot::FileProvider> fileProvider,
                           std::unique_ptr<cspot::AudioDecoder> audioDecoder)
    : bell::Task("cspot_player", 4 * 1024),
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

  std::cout << "Handling queue update, current track id: "
            << (update.currentTrackId ? update.currentTrackId->uri : "none")
            << ", next tracks: ";
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

  // Ensure we don't keep files for tracks that are no longer in the queue
  for (auto& trackId : providedTracks) {
    if (std::find(playbackQueue.begin(), playbackQueue.end(), trackId.first) ==
        playbackQueue.end()) {
      providedTracks.erase(trackId.first);
      fileProvider->cancel(trackId.first);
    }
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
    std::string audioKey = base64Encode(providedFile.decryptionKey.data(),
                                        providedFile.decryptionKey.size());

    BELL_LOG(info, LOG_TAG,
             "Track {} is ready to play from file {}, audio key = {}",
             providedFile.itemId.uri, providedFile.cdnUrl, audioKey);

    if (providedFile.itemId == playbackQueue[0]) {
      announceState();
    }
  } else {
    // Probably outdated request
  }
}

void StreamPlayer::handlePlayEvent(bool shouldPlay) {
  std::scoped_lock lock(playbackMutex);
}

void StreamPlayer::handleFlushEvent() {
  std::scoped_lock lock(playbackMutex);
  flushRequested = true;
}

void StreamPlayer::announceState() {
  std::scoped_lock lock(playbackMutex);

  PlayerStateUpdate stateUpdate{
      .isPlaying = false,
      .isBuffering = true,
      .timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count(),
      .positionAsOfTimestamp = 0,
      .playbackDurationMs = 0,
  };

  if (providedTracks[playbackQueue[0]].trackMetadata) {
    stateUpdate.playbackDurationMs =
        providedTracks[playbackQueue[0]].trackMetadata->durationMs;
    stateUpdate.isPlaying = true;
    stateUpdate.isBuffering = false;
  }

  eventLoop->post(EventLoop::EventType::PLAYER_STATE_UPDATED, stateUpdate);
}

void StreamPlayer::taskLoop() {
    bell::utils::sleepMs(100);
  // if (!trackDecoder->isOpen()) {
  //   // In case there's no stream, wait for semaphore
  //   queueUpdateSemaphore.take(100);
  // }

  // std::optional<ProvidedFile> requestedProvidedFile;
  // {
  //   std::scoped_lock lock(playbackMutex);

  //   // Got a flush, reset state
  //   if (flushRequested) {
  //     BELL_LOG(info, LOG_TAG, "Flush requested, resetting state");
  //     flushRequested = false;
  //     trackDecoder->resetStream();
  //     currentTrackIndex = 0;
  //   }

  //   if (!trackDecoder->isOpen() && isCurrentTrackReady()) {
  //     requestedProvidedFile = providedTracks[playbackQueue[currentTrackIndex]];
  //   }
  // }

  // if (requestedProvidedFile) {
  //   BELL_LOG(info, LOG_TAG, "Starting playback of track {}",
  //            requestedProvidedFile->itemId.uri);

  //   auto res = trackDecoder->open(requestedProvidedFile->cdnUrl,
  //                                 requestedProvidedFile->decryptionKey);
  //   if (!res) {
  //     BELL_LOG(error, LOG_TAG, "Failed to open CDN stream: {}", res.error());
  //     return;
  //   }
  // }

  // if (trackDecoder->isOpen()) {
  //   auto res = trackDecoder->decodePacket();
  //   if (!res) {
  //     BELL_LOG(error, LOG_TAG, "Failed to decode packet: {}", res.error());
  //   } else {
  //   }
  //   bell::utils::sleepMs(2);  // Simulate some processing time

  //   if (trackDecoder->isEndOfStream()) {
  //     BELL_LOG(info, LOG_TAG, "Track ended, moving to next track");
  //     std::scoped_lock lock(playbackMutex);
  //     trackDecoder->resetStream();
  //     currentTrackIndex++;
  //   }
  // }
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
