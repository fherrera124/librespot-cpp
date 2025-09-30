#pragma once

#include <trompeloeil.hpp>

#include "bell/Result.h"
#include "tracks/AudioDecoder.h"

// Mock for FileProvider class
class MockAudioDecoder : public cspot::AudioDecoder {
 public:
  MAKE_MOCK2(openStream,
             bell::Result<>(const std::string&, const std::vector<std::byte>&),
             override);

  MAKE_MOCK0(processPacket, void(), override);

  MAKE_MOCK0(resetStream, void(), override);

  MAKE_CONST_MOCK0(isOpen, bool(), override);

  MAKE_CONST_MOCK0(isEOF, bool(), override);
};
