#pragma once

// Standard includes
#include <memory>
#include <vector>

#include "api/ApClient.h"
#include "api/SpClient.h"
#include "proto/MetadataPb.h"
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
    std::shared_ptr<ApClient> apClient,
    std::vector<AudioFormat> qualityPreference = {AudioFormat_OGG_VORBIS_320,
                                                   AudioFormat_OGG_VORBIS_160,
                                                   AudioFormat_OGG_VORBIS_96});

}  // namespace cspot
