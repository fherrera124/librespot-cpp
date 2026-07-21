#include "PlainI2SAudioSink.h"

PlainI2SAudioSink::PlainI2SAudioSink(const Config& config) {
  initI2sChannel(config);
  startI2sFeed();
}
