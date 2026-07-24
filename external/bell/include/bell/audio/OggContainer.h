#pragma once

#include "bell/audio/Common.h"
#include "bell/audio/Container.h"

#include "ogg/ogg.h"

namespace bell::audio {
class OggContainer : public Container {
 public:
  OggContainer() = default;
  ~OggContainer() override{};

  // Base class overrides
  bell::Result<> openForRead(
      std::shared_ptr<bell::io::DataStream> dataStream) override;
  bell::Result<EncodedPacket> readNextPacket() override;
  bell::Result<> seekToFrame(size_t frameIndex,
                             size_t allowedDistance = 0) override;
  uint64_t tellFrame() const override;
  uint64_t getTotalFrames() override;

 private:
  const char* LOG_TAG = "OggContainer";

  std::shared_ptr<bell::io::DataStream> stream;
  ogg_stream_state oggStreamState;
  ogg_sync_state oggSyncState;
  ogg_page oggPage;
  ogg_packet packet;
  uint64_t totalFrames;
  uint64_t currentFrame = 0;
  int64_t dataStartOffset = 0;
  int streamSerialNo;  // TODO: support multiple streams

  bell::Result<> readNextPage();

  void close();
};
};  // namespace bell::audio
