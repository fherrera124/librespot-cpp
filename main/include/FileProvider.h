#pragma once

// Standard includes
#include <memory>

// Library includes
#include "bell/utils/Semaphore.h"
#include "bell/utils/Task.h"

#include "CDNAudioStream.h"
#include "SessionContext.h"
#include "TrackQueue.h"
#include "api/ApClient.h"
#include "api/SpClient.h"
#include "events/EventModels.h"
#include "proto/SpotifyId.h"

namespace cspot {
class FileProvider : public bell::Task {
 public:
  FileProvider(std::shared_ptr<SessionContext> sessionContext,
               std::shared_ptr<SpClient> spClient,
               std::shared_ptr<ApClient> apClient);

  // Submits a track for providing, will return a ProvidedFile through the event loop
  void provideTrack(const SpotifyId& trackId);

  // Cancels providing a track by its ID
  void cancel(const SpotifyId& trackId);

 private:
  const char* LOG_TAG = "FileProvider";

  std::shared_ptr<SessionContext> sessionContext;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<ApClient> apClient;

  std::mutex providedFilesMutex;
  bell::Semaphore providedFileSemaphore;
  std::vector<ProvidedFile> currentlyProvidedFiles;

  std::mutex pendingAudioKeyFilesMutex;
  std::unordered_map<SpotifyId, ProvidedFile> pendingAudioKeyFiles;

  void taskLoop() override;

  void handleAudioKeyResponse(const AudioKeyResponse& response);
};
}  // namespace cspot
