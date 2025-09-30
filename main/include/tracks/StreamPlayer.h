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
  std::unique_ptr<AudioDecoder> audioDecoder;

  void taskLoop() override;

  void registerHandlers();
  void handleQueueUpdate(const TrackQueueUpdate& queueUpdate);
  void handleFileProvided(const ProvidedFile& providedFile);
  bool isCurrentTrackReady();
  void handlePlayEvent(bool play);
  void handleFlushEvent();

  void announceState();
};
}  // namespace cspot
