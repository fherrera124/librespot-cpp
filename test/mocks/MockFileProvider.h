#pragma once

#include <trompeloeil.hpp>

#include "FileProvider.h"
#include "proto/SpotifyId.h"

// Mock for FileProvider class
class MockFileProvider : public cspot::FileProvider {
 public:
  MAKE_MOCK1(provideTrack, void(const cspot::SpotifyId&), override);

  MAKE_MOCK1(cancel, void(const cspot::SpotifyId&), override);
};
