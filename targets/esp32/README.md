# ESP32 CSpot integration

CSpot requires an ESP32 board with PSRAM. It has been tested on the
AI-Thinker ESP32 Audio Kit, ESP32-WROVER and LOLIN S2 Mini. Configure Wi-Fi
credentials with `idf.py menuconfig` or an ignored `sdkconfig.defaults.local`.

## LOLIN S2 Mini

This profile targets the LOLIN S2 Mini with 4 MB flash, 2 MB PSRAM and a
PCM5102A DAC connected to BCLK=10, WS=11 and DOUT=12. From this directory,
with the ESP-IDF environment loaded, run:

```sh
idf.py -B build.lolin-s2 -DIDF_TARGET=esp32s2 \
  -DSDKCONFIG=sdkconfig.lolin-s2 \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/lolin-s2-mini.defaults' build
```

The generated build directory and `sdkconfig.lolin-s2` are local files. Delete
the latter when board defaults change and a fresh configuration is required.
