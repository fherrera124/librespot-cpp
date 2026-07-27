# librespot-cpp ESP32 example

A minimal ESP32 Spotify Connect speaker, built on top of
`cspot::SpotifyConnectReceiver` - the same class `extras/cli` (this repo,
host/desktop) uses. It advertises itself via mDNS/ZeroConf; select it
from the Spotify app to start playback.

Targets the JC3248W535 board (ESP32-S3 + onboard mono I2S amp) by
default - see `main/Kconfig.projbuild` to point the I2S pins at
different hardware.

This is the minimum needed to hear audio on a real device: WiFi
bootstrap, an I2S sink, and the receiver itself. For a fuller example
with an LVGL UI and on-device transport controls, see the separate
`cspot` project's `components/cspot/cspot_connect.cpp`, which wraps this
same class in a C API for that purpose.

No session persistence - like `extras/cli`, this re-pairs via ZeroConf
on every boot (see `main/main.cpp`'s own comment on why).

## Prerequisites

ESP-IDF v5.x, `IDF_PATH`/environment set up (`. $IDF_PATH/export.sh` or
equivalent). No Spotify Developer Dashboard app needed - pairing is
ZeroConf-only, and the same user-session token it gets from that also
covers fetching track audio from the CDN.

## Building

```bash
cd extras/esp32
idf.py set-target esp32s3
idf.py menuconfig   # set WiFi SSID/password (Example Connection Configuration)
idf.py build flash monitor
```

`idf.py` resolves the `librespot-cpp` component from this checkout's own
repo root (see this directory's `CMakeLists.txt`) - no network fetch, no
separate clone needed to try local changes.

## Running

Once flashed, watch the serial monitor for the WiFi connect and "Spotify
Connect receiver started" log lines, then open the Spotify app on the
same network and select the device by name (`CSPOT_DEVICE_NAME`, default
`"librespot-cpp ESP32"`).
