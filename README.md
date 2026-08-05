![C/C++ CI](https://github.com/feelfreelinux/cspot/workflows/C/C++%20CI/badge.svg)
![ESP IDF](https://github.com/feelfreelinux/cspot/workflows/ESP%20IDF/badge.svg)
[![Certification](https://badgen.net/badge/Stary%20Filipa/certified?color=purple)](https://github.com/feelfreelinux/cspot)
[![Certification](https://badgen.net/badge/Sasin/stole%2070%20mln%20PLN)](https://github.com/feelfreelinux/cspot)

<p align="center">
<img src=".github/trombka.png" width="32%" />
</p>

# :trumpet: cspot

A Spotify Connect player written in CPP targeting, but not limited to embedded devices (ESP32).

Currently in state of rapid development.

*Only to be used with premium spotify accounts*

## Building

### Prerequisites

Summary:

- cmake (version 3.0 or higher)
- gcc / clang for the CLI target
- [esp-idf](https://github.com/espressif/esp-idf) for building for the esp32
- portaudio for playback on MacOS
- downloaded submodules
- golang (1.16)
- protoc
- on Linux you will additionally need:
    - `libasound` and `libavahi-compat-libdnssd`
- mbedtls

This project utilizes submodules, please make sure you are cloning with the `--recursive` flag or use `git submodule update --init --recursive`.

MBedTLS is now the sole option, so you can get it from [there](https://github.com/Mbed-TLS/mbedtls) and rebuild it or have it installed system-wide using your favorite package manager. See below how to use a local version.

This library uses nanopb to generate c files from protobuf definitions. Nanopb itself is included via submodules, but it requires a few external python libraries to run the generators.

To install them you can use pip:

```shell
$ sudo pip3 install protobuf grpcio-tools
```

(You probably should use venv, but I am no python developer)

To install avahi and asound dependencies on Linux you can use:

```shell
$ sudo apt-get install libavahi-compat-libdnssd-dev libasound2-dev
```


### Building for macOS
### Building for macOS/Linux & Windows

The cli target is used mainly for testing and development purposes, as of now it has the same features as the esp32 target.

As MbedTLS is now use instead of OpenSSL, you need to install it or your system or have a local build. If you have a system-wide install of MbedTLS, ignore what's below

To use a local build, you have to specify the BELL_EXTERNAL_MBEDTLS and potentially MBEDTLS_RELEASE. The first one points to the "./cmake" subdir of the MbedTLS's build directory, the second optionally defines the name of the MbedTLS build (it's by default set to 'RELEASE' for Windows and 'NOCONFIG' for others). 

See running the CLI for information on how to run cspot on a desktop computer.

#### macOS/Linux

```shell
# navigate to the targets/cli directory
$ cd targets/cli

# create a build directory and navigate to it
$ mkdir -p build && cd build

# use cmake to generate build files, and select an audio sink
$ cmake .. -DUSE_PORTAUDIO=ON [-DBELL_EXTERNAL_MBEDTLS=<mbedtls_build_dir>/cmake>] [-DMBEDTLS_RELEASE=<release_name>]

# compile
$ make 
```

#### Windows

```shell
# navigate to the targets/cli directory
$ cd targets/cli

# create a build directory and navigate to it
$ mkdir -p build && cd build

# use cmake to generate build files, and select an audio sink
$ cmake .. -A Win32|x64 -DUSE_PORTAUDIO=ON [-DBELL_EXTERNAL_MBEDTLS=<mbedtls_build_dir>/cmake>] [-DMBEDTLS_RELEASE=<release_name>]
```

Go to `build` and use `cspotcli.sln` under VisualStudio or use `msbuild` from command line.

Note that for now, only the Win32 build has been tested, not the x64 version. Under some VS releases, the protobuf might not be rebuilt automatically, just go to the project "generate_proto_sources" and do a C^F7 on each `*.pb.rule`

### Building for Linux

The cli target is used mainly for testing and development purposes, as of now it has the same features as the esp32 target.

```shell
# navigate to the targets/cli directory
$ cd targets/cli

# create a build directory and navigate to it
$ mkdir -p build && cd build

# use cmake to generate build files, and select an audio sink
$ cmake .. -DUSE_ALSA=ON

# compile
$ make 
```
See running the CLI for information on how to run cspot on a desktop computer.

### Building for ESP32

The ESP32 target is built using the [esp-idf](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) toolchain

```shell
# Follow the instructions for setting up esp-idf for your operating system, up to `. ./export.sh` or equivalent
# esp-idf has a Python virtualenv, install nanopb's dependencies in it
$ pip3 install protobuf grpcio-tools
# update submodules after each code pull to avoid build errors
$ git submodule update --init --recursive
# navigate to the targets/esp32 directory
$ cd targets/esp32
# run once after pulling the repo
$ idf.py set-target esp32
```

Configure CSPOT according to your hardware

```shell
# run visual config editor, when done press Q to save and exit
$ idf.py menuconfig
```

Navigate to `Example Connection Configuration` and provide wifi connection details

![idf-menuconfig](/targets/esp32/doc/img/idf-menuconfig-2.png)

Navigate to `CSPOT Configuration`, you may configure device name, output device and audio quality.

![idf-menuconfig](/targets/esp32/doc/img/idf-menuconfig-1.png)

#### Building and flashing

Build and upload the firmware

```shell
# compile
$ idf.py build

# upload
$ idf.py flash
```
The ESP32 will restart and begin running cspot. You can monitor it using a serial console.

Optionally run as single command

```shell
# compile, flash and attach monitor
$ idf.py build flash monitor
```

## Running

## The CLI version

After building the app, the only thing you need to do is to run it through CLI.

```shell
$ ./cspotcli

```
If you run it with no parameter, it will use ZeroConf to advertise itself. This means that until at least one **local** Spotify Connect application has discovered and connected it, it will not be registered to Spotify servers. As a consequence, Spotify's WebAPI will not be able to see it. If you want the player to be registered at start-up, you need to at least once to create a credentials file and then re-use that file. Run it with -u/-p/-c once and then run it with -c only. See command's line help.

Now open a real Spotify app and you should see a cspot device on your local network. Use it to play audio.


# Architecture

## External interface

`cspot` is meant to be used as a lightweight C++ library for playing back Spotify music and receive control notifications from Spotify connect. 
It exposes an interface for starting the communication with Spotify servers and expects the embedding program to provide an interface for playing back raw audio samples ([`AudioSink`](include/AudioSink.h)).

You can view the [`cspot-cli`]([targets/cli/main.cpp) program for a reference on how to include cspot in your program. It provides a few audio sinks for various platforms and uses:

- [`ALSAAudioSink`](cspot/bell/src/sinks/unix/ALSAAudioSink.cpp) - Linux, requires `libasound`
- [`PortAudioSink`](cspot/bell/src/sinks/unix/PortAudioSink.cpp) - MacOS (PortAudio also supports more platforms, but we currently use it only on MacOS), requires the PortAudio library
- [`NamedPipeAudioSink`](cspot/bell/src/sinks/unix/NamedPipeAudioSink.cpp) - all platforms, writes to a file/FIFO pipe called `outputFifo` which can later be played back by FFmpeg. Used mainly for testing and development.

Additionaly the following audio sinks are implemented for the esp32 target:
- [`ES9018AudioSink`](cspot/bell/src/sinks/esp/ES9018AudioSink.cpp) - provides playback via a ES9018 DAC connected to the ESP32
- [`AC101AudioSink`](cspot/bell/src/sinks/esp/AC101AudioSink.cpp) - provides playback via the AC101 DAC used in cheap ESP32 A1S audiokit boards, commonly found on aliexpress.
- [`PCM5102AudioSink`](cspot/bell/src/sinks/esp/PCM5102AudioSink.cpp) - provides playback via a PCM5102 DAC connected to the ESP32, commonly found in the shape of small purple modules at various online retailers. Wiring can be configured in the sink and defaults to:
  - SCK to Ground
  - BCK to PGIO27
  - DIN to GPIO25
  - LCK to GPIO32
  - GND to Ground
  - VIN to 3.3V (but supposedly 5V tolerant)
- TODO: internal esp32 DAC for crappy quality testing.

You can also easily add support for your own DAC of choice by implementing your own audio sink. Each new audio sink must implement the `void feedPCMFrames(std::vector<uint8_t> &data)` method which should accept stereo PCM audio data at 44100 Hz and 16 bits per sample. Please note that the sink should somehow buffer the data, because playing it back may result in choppy audio.

An audio sink can optionally implement the `void volumeChanged(uint16_t volume)` method which is called everytime the user changes the volume (for example via Spotify Connect). If an audio sink implements it it should set `softwareVolumeControl` to `false` in its constructor to let cspot know to disable the software volume adjustment. Properly implementing external volume control (for example via dedicated hardware) will result in a better playback quality since all the dynamic range is used to encode the samples.

The embedding program should also handle caching the authentication data, so that the user does not have to authenticate via the local network (Zeroconf) each time cspot is started. For reference on how to do it please refer to the `cspot-cli` target (It stores the data in `authBlob.json`). 

## Tuning the CDN fetch pipeline for your hardware

Audio is fetched from Spotify's CDN in byte ranges instead of one continuous stream, with a background worker trying to keep some of them fetched ahead of playback. Both that and which Vorbis quality to prefer are grouped into [`cspot::AudioConfig`](main/include/Session.h), passed to `Session`'s constructor - the embedding program's `main.cpp` can override the defaults per board:

```cpp
struct AudioConfig {
  std::chrono::milliseconds targetPrefetchDuration{6500};
  std::vector<AudioFormat> qualityPreference = {
      AudioFormat_OGG_VORBIS_320, AudioFormat_OGG_VORBIS_160, AudioFormat_OGG_VORBIS_96};
};
```

Each CDN fetch is a fixed `kCDNChunkSize` (32KB, [`CDNDataStream.h`](main/include/audio/CDNDataStream.h)) - same request size and RAM cost per chunk no matter what quality gets resolved. What varies per track instead is **how many** chunks the background worker keeps fetched ahead (`prefetchDepth`), derived inside [`AudioDecoderImpl::openStream()`](main/src/audio/AudioDecoderImpl.cpp) from `targetPrefetchDuration` and whichever quality `FileProvider` actually resolved for that track (it tries `qualityPreference` in order - a track missing the top choice falls back, so the resolved quality can end up lower than requested):

```
prefetchDepth = ceil(targetPrefetchDuration_ms × bytesPerSecond(resolved_quality) / 1000 / kCDNChunkSize)
```

This keeps the real-world buffered duration roughly constant regardless of quality - a 320kbps track needs more (smaller-in-time) chunks to cover the same ~6.5s than a 96kbps one does, instead of both getting the same chunk count covering very different durations.

### On PSRAM (ESP32/ESP32-S3 only)

PSRAM is external RAM wired next to the SoC (typically 2-8MB on boards that have it) - separate from, and much bigger than, the chip's own internal SRAM (a few hundred KB). **This library's ESP32 target requires a board with PSRAM.** `bell::Task` - the base class behind every long-running component here (`AudioSinkI2S`, `PrefetchWorker`, `StreamPlayer`, `FileProvider`, `EventLoop`, ...) - allocates task stacks from PSRAM by default, and task creation fails outright without it. A plain ESP32/ESP32-S3 with no PSRAM chip cannot run this library as configured out of the box; see `targets/esp32/sdkconfig.defaults`' `CONFIG_SPIRAM*` options.

Everything sized below (`ChunkCache`, `lastReadChunk`) lives in this same PSRAM - the SoC's much smaller internal SRAM is reserved for things that must be internal (DMA descriptors, etc. - `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` in that same file). "RAM budget" in this section means PSRAM headroom, not total chip RAM.

### RAM ceiling is fixed, not quality-dependent

Because `kCDNChunkSize` is a constant, the RAM ceiling is bounded by `kChunkCacheCapacity` (`CDNDataStream.cpp`, currently `9`) alone - **it no longer scales with `targetPrefetchDuration` or with which quality gets resolved**. The only effect of those two is how much of that fixed ceiling `prefetchDepth` actually uses.

| Buffer | Size |
|---|---|
| `ChunkCache` (up to `kChunkCacheCapacity` chunks resident at once) | `kChunkCacheCapacity × kCDNChunkSize` |
| `CDNDataStream::lastReadChunk` (one per currently-open track) | `1 × kCDNChunkSize` |
| Transient (foreground fetch and prefetch worker both in flight at once - freed as each publishes) | up to `2 × kCDNChunkSize` |

**Steady-state resident: `(kChunkCacheCapacity + 1) × kCDNChunkSize` = `10 × 32KB` ≈ 320KB. Peak (incl. transient): ≈ 384KB.** Fixed numbers, true for every quality.

### The real trade-off: does `prefetchDepth` fit inside `kChunkCacheCapacity`?

`kChunkCacheCapacity` doesn't auto-adjust to your config - if you raise `targetPrefetchDuration` or add a higher bitrate to `qualityPreference` without also raising `kChunkCacheCapacity` to match, the extra depth silently gets capped (`ChunkCache::claim()` returns `WindowFull`, degrading how far ahead prefetch reaches - never a correctness issue, just less cushion against Wi-Fi jitter than requested). At the current default (`targetPrefetchDuration=6.5s`, `kChunkCacheCapacity=9`):

| Resolved quality | Bitrate | ms/chunk | `prefetchDepth` | Fits in capacity 9? |
|---|---|---|---|---|
| `OGG_VORBIS_96` | 12KB/s | ~2730ms | 3 | yes, 6 slots to spare |
| `OGG_VORBIS_160` | 20KB/s | ~1640ms | 4 | yes, 5 slots to spare |
| `OGG_VORBIS_320` | 40KB/s | ~820ms | 8 | yes, 1 slot to spare |

### Picking values for your chip

1. Decide `targetPrefetchDuration` and your `qualityPreference`'s **highest** bitrate (the worst case for depth).
2. Compute the worst-case depth with the formula above, and set `kChunkCacheCapacity` to at least `worst_case_depth + 1` (one slot of margin for the foreground path's own claim).
3. RAM ceiling follows directly: `(kChunkCacheCapacity + 1) × 32KB`, plus ≈64KB transient - budget against your board's free PSRAM.

A longer `targetPrefetchDuration` is most worth it on high-RTT/high-latency links (WiFi with a distant AP, a congested channel, or a CDN edge that's far away) - if round-trip time to your CDN is already low, the default may already be more margin than you need.

## Internal details

The connection with Spotify servers to play music and recieve control information is pretty complex. First of all an access point address must be fetched from Spotify ([`ApResolve`](cspot/src/ApResolve.cpp) fetches the list from http://apresolve.spotify.com/). Then a [`PlainConnection`](cspot/include/PlainConnection.h) with the selected Spotify access point must be established. It is then upgraded to an encrypted [`ShannonConnection`](cspot/include/ShannonConnection.h).
