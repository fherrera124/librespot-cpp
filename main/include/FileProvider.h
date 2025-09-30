#pragma once

// Standard includes
#include <memory>

#include "api/ApClient.h"
#include "api/SpClient.h"
#include "proto/SpotifyId.h"

namespace cspot {
class FileProvider {
 public:
  virtual ~FileProvider() = default;

  // Submits a track for providing, will return a ProvidedFile through the event loop
  virtual void provideTrack(const SpotifyId& trackId) = 0;

  // Cancels providing a track by its ID
  virtual void cancel(const SpotifyId& trackId) = 0;
};

std::unique_ptr<FileProvider> createDefaultFileProvider(
    std::shared_ptr<EventLoop> eventLoop, std::shared_ptr<SpClient> spClient,
    std::shared_ptr<ApClient> apClient);

}  // namespace cspot
