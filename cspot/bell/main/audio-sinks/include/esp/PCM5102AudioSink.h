#ifndef PCM5102AUDIOSINK_H
#define PCM5102AUDIOSINK_H

#include "BufferedAudioSink.h"

// Generic plain I2S DAC/amp sink - no control interface (I2C/SPI) needed,
// just BCLK/WS/DOUT(+MCLK). "PCM5102" is a historical name; nothing here
// is specific to that chip. See docs/spotify_component_analysis.md,
// findings F9/F51.
class PCM5102AudioSink : public BufferedAudioSink {
 public:
  explicit PCM5102AudioSink(const Config& config = Config());
  ~PCM5102AudioSink() override = default;
};

#endif
