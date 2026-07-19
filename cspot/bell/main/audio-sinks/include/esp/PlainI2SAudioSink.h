#ifndef PLAINI2SAUDIOSINK_H
#define PLAINI2SAUDIOSINK_H

#include "BufferedAudioSink.h"

// Plain I2S DAC/amp sink - no control interface (I2C/SPI) needed, just
// BCLK/WS/DOUT(+MCLK). Contrast with ES8388AudioSink/AC101AudioSink/etc,
// which do need one.
class PlainI2SAudioSink : public BufferedAudioSink {
 public:
  explicit PlainI2SAudioSink(const Config& config = Config());
  ~PlainI2SAudioSink() override = default;
};

#endif
