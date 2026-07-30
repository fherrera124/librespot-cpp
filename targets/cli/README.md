# CLI CSpot integration

Integrates CSpot with macOS / Linux via a simple command line target. Mainly
used for testing and development.

## Prerequisites

- CMake 3.18+ and a C++20 compiler
- Git submodules checked out: `git submodule update --init --recursive`
  (this also brings in mbedTLS, built from source automatically - no system
  package needed)
- On Linux: ALSA and Avahi development headers (`libasound2-dev libavahi-client-dev`
  on Debian/Ubuntu)
- On Linux: `avahi-daemon` running, so the Zeroconf/mDNS advertise used for
  pairing actually reaches the network

## Building

From the repository root:

```shell
mkdir -p build && cd build
cmake -DCSPOT_TARGET_CLI=ON ..
cmake --build . --target cspot_cli
```

The binary is produced at `build/targets/cli/cspot_cli`.

## Running

```shell
./cspot_cli
```

It advertises itself over Zeroconf/mDNS - open the Spotify app on the same
local network and select the device to pair. Once paired, the session
(device id and login credentials) is persisted to `session.json` in the
current working directory and reused on the next run, so pairing is only
needed once.
