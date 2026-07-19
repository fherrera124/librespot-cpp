#include "PCM5102AudioSink.h"

// Ported to driver/i2s_std.h - was hardcoded to bck_io_num=27/
// ws_io_num=32/data_out_num=25 on I2S port 0 with no way to configure them
// (finding F9); now takes a Config the same shape as this project's
// example app already used for its own (now-retired) sink. See
// docs/spotify_component_analysis.md, finding F51.
PCM5102AudioSink::PCM5102AudioSink(const Config& config) {
  initI2sChannel(config);
  startI2sFeed();
}
