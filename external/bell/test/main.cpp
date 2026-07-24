#include "bell/Logger.h"
#include "bell/audio/OggContainer.h"
#include "bell/audio/TremorVorbisCodec.h"

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

int main(int argc, char* argv[]) {
  bell::registerDefaultLogger();

  doctest::Context context;
  context.applyCommandLine(argc, argv);

  int res = context.run();  // run doctest

  // important - query flags (and --exit) rely on the user doing this
  if (context.shouldExit()) {
    return res;
  }

  return 0;
}
