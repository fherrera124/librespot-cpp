[![Build cspot-cli on Ubuntu](https://github.com/fherrera124/librespot-cpp/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/fherrera124/librespot-cpp/actions/workflows/c-cpp.yml)
[![Run clang-format check](https://github.com/fherrera124/librespot-cpp/actions/workflows/clang-format.yml/badge.svg)](https://github.com/fherrera124/librespot-cpp/actions/workflows/clang-format.yml)

<p align="center">
<img src=".github/trombka.png" width="32%" />
</p>

# librespot-cpp

A Spotify Connect player written in C++ targeting, but not limited to, embedded devices (ESP32). Fork of [feelfreelinux/cspot](https://github.com/feelfreelinux/cspot) - the C++ namespace and public API are still named `cspot`.

Currently in a state of rapid development.

*Only to be used with premium Spotify accounts*

## Spotify Connect engine

The engine (`Session`, `DealerSession`, `PlayerEngine`, `TrackPlayer`,
`TrackQueue`, `AudioSink`) has no application glue (WiFi, zeroconf, I2S
pins) - that's the consuming project's job.

### Consuming via plain CMake (Linux/macOS/Win32, or `extras/cli`)

A normal `add_subdirectory()` - see `extras/cli/CMakeLists.txt` for
reference.

### Consuming via ESP-IDF, through Component Manager

The root `CMakeLists.txt` is dual-mode: under `ESP_PLATFORM` it registers
as a real component (`idf_component_register()`) instead of a plain
`add_library()`. In the consuming component's `idf_component.yml`:

```yaml
dependencies:
  librespot_cpp:
    git: "https://github.com/fherrera124/librespot-cpp.git"
```

(no `path:` needed - the whole repo is the component, following the
Pitchfork Layout restructuring; it used to point at `path: "cspot"`.)

Then, in that same component's `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "my_connect.cpp"
    INCLUDE_DIRS "include"
)
```

No need to declare `REQUIRES librespot_cpp` explicitly - Component
Manager already adds the manifest's dependencies as a requirement of the
component.

#### Options (`idf.py menuconfig` → "librespot-cpp (Spotify Connect engine)")

Codecs (`LIBRESPOT_CODEC_VORBIS`/`MP3`/`AAC`/`OPUS`/`ALAC`), and toggles
for `bell`'s internal MQTT/HTTP server and the JSON backend (cJSON vs.
nlohmann_json) - see `Kconfig` at the repo root for the defaults.

## Building

### Prerequisites

Summary:

- cmake (version 3.16 or higher)
- gcc / clang for the CLI target
- [esp-idf](https://github.com/espressif/esp-idf) for building for the esp32 (consumed via Component Manager - see above)
- portaudio for playback on MacOS
- protoc
- on Linux you will additionally need:
    - `libasound` and `libavahi-compat-libdnssd`
- mbedtls

Everything this library vendors (`external/bell`, nanopb, etc.) is committed directly - no git submodules, no `--recursive` clone needed.

MBedTLS is now the sole option, so you can get it from [there](https://github.com/Mbed-TLS/mbedtls) and rebuild it or have it installed system-wide using your favorite package manager. See below how to use a local version.

This library uses nanopb to generate c files from protobuf definitions. Nanopb itself is vendored under `external/bell/external/nanopb`, but it requires a few external python libraries to run the generators.

To install them you can use pip:

```shell
$ sudo pip3 install protobuf grpcio-tools
```

(You probably should use venv, but I am no python developer)

To install avahi and asound dependencies on Linux you can use:

```shell
$ sudo apt-get install libavahi-compat-libdnssd-dev libasound2-dev
```


### Building for macOS/Linux & Windows

The CLI target is used mainly for testing and development purposes.

As MbedTLS is now used instead of OpenSSL, you need to install it on your system or have a local build. If you have a system-wide install of MbedTLS, ignore what's below

To use a local build, you have to specify the BELL_EXTERNAL_MBEDTLS and potentially MBEDTLS_RELEASE. The first one points to the "./cmake" subdir of the MbedTLS's build directory, the second optionally defines the name of the MbedTLS build (it's by default set to 'RELEASE' for Windows and 'NOCONFIG' for others). 

See running the CLI for information on how to run cspot on a desktop computer.

#### macOS/Linux

```shell
# navigate to the extras/cli directory
$ cd extras/cli

# create a build directory and navigate to it
$ mkdir -p build && cd build

# use cmake to generate build files, and select an audio sink
$ cmake .. -DUSE_PORTAUDIO=ON [-DBELL_EXTERNAL_MBEDTLS=<mbedtls_build_dir>/cmake>] [-DMBEDTLS_RELEASE=<release_name>]

# compile
$ make 
```

#### Windows

```shell
# navigate to the extras/cli directory
$ cd extras/cli

# create a build directory and navigate to it
$ mkdir -p build && cd build

# use cmake to generate build files, and select an audio sink
$ cmake .. -A Win32|x64 -DUSE_PORTAUDIO=ON [-DBELL_EXTERNAL_MBEDTLS=<mbedtls_build_dir>/cmake>] [-DMBEDTLS_RELEASE=<release_name>]
```

Go to `build` and use `cspotcli.sln` under VisualStudio or use `msbuild` from command line.

Note that for now, only the Win32 build has been tested, not the x64 version. Under some VS releases, the protobuf might not be rebuilt automatically, just go to the project "generate_proto_sources" and do a C^F7 on each `*.pb.rule`

### Building for Linux

The CLI target is used mainly for testing and development purposes.

```shell
# navigate to the extras/cli directory
$ cd extras/cli

# create a build directory and navigate to it
$ mkdir -p build && cd build

# use cmake to generate build files, and select an audio sink
$ cmake .. -DUSE_ALSA=ON

# compile
$ make 
```
See running the CLI for information on how to run cspot on a desktop computer.

### Building for ESP32

This repo has no standalone ESP32 example project of its own anymore -
it's meant to be consumed as an ESP-IDF component by your own
application, via Component Manager (see "Consuming via ESP-IDF" above).
Once your app declares the dependency, the usual ESP-IDF flow applies in
*your* project:

```shell
$ idf.py set-target esp32   # or esp32s3, etc.
$ idf.py menuconfig         # configure your app + "librespot-cpp (Spotify Connect engine)"
$ idf.py build flash monitor
```

Status LED indication, GPIO/DAC selection, WiFi credentials, etc. are
all your consuming app's concern, not this library's.

## Running

### The CLI version

After building the app, the only thing you need to do is to run it through CLI.

```shell
$ ./cspotcli

```
If you run it with no parameter, it will use ZeroConf to advertise itself. This means that until at least one **local** Spotify Connect application has discovered and connected it, it will not be registered to Spotify servers. As a consequence, Spotify's WebAPI will not be able to see it. If you want the player to be registered at start-up, you need to either use username/password all the time or at least once to create a credentials file and then re-use that file. Run it with -u/-p/-c once and then run it with -c only. See command's line help.

Now open a real Spotify app and you should see a cspot device on your local network. Use it to play audio.


# Architecture

## External interface

`cspot` is meant to be used as a lightweight C++ library for playing back Spotify music and receive control notifications from Spotify connect. 
It exposes an interface for starting the communication with Spotify servers and expects the embedding program to provide an interface for playing back raw audio samples ([`AudioSink`](external/bell/main/audio-sinks/include/AudioSink.h)).

You can view the [`cspot-cli`](extras/cli/main.cpp) program for a reference on how to include cspot in your program. It provides a few audio sinks for various platforms and uses:

- [`ALSAAudioSink`](external/bell/main/audio-sinks/unix/ALSAAudioSink.cpp) - Linux, requires `libasound`
- [`PortAudioSink`](external/bell/main/audio-sinks/unix/PortAudioSink.cpp) - MacOS (PortAudio also supports more platforms, but we currently use it only on MacOS), requires the PortAudio library
- [`NamedPipeAudioSink`](external/bell/main/audio-sinks/unix/NamedPipeAudioSink.cpp) - all platforms, writes to a file/FIFO pipe called `outputFifo` which can later be played back by FFmpeg. Used mainly for testing and development.

Additionally, the following audio sinks are implemented for the ESP32 target (a few more than listed here exist under `external/bell/main/audio-sinks/esp/` - `BufferedAudioSink`, `ES8311AudioSink`, `ES8388AudioSink`, `TAS5711AudioSink`, `SPDIFAudioSink`, `InternalAudioSink`, `PlainI2SAudioSink`):
- [`ES9018AudioSink`](external/bell/main/audio-sinks/esp/ES9018AudioSink.cpp) - provides playback via a ES9018 DAC connected to the ESP32
- [`AC101AudioSink`](external/bell/main/audio-sinks/esp/AC101AudioSink.cpp) - provides playback via the AC101 DAC used in cheap ESP32 A1S audiokit boards, commonly found on aliexpress.
- [`PCM5102AudioSink`](external/bell/main/audio-sinks/esp/PCM5102AudioSink.cpp) - provides playback via a PCM5102 DAC connected to the ESP32, commonly found in the shape of small purple modules at various online retailers. Wiring can be configured in the sink and defaults to:
  - SCK to Ground
  - BCK to PGIO27
  - DIN to GPIO25
  - LCK to GPIO32
  - GND to Ground
  - VIN to 3.3V (but supposedly 5V tolerant)

You can also easily add support for your own DAC of choice by implementing your own audio sink. Each new audio sink must implement the `void feedPCMFrames(std::vector<uint8_t> &data)` method which should accept stereo PCM audio data at 44100 Hz and 16 bits per sample. Please note that the sink should somehow buffer the data, because playing it back may result in choppy audio.

An audio sink can optionally implement the `void volumeChanged(uint16_t volume)` method which is called every time the user changes the volume (for example via Spotify Connect). If an audio sink implements it, it should set `softwareVolumeControl` to `false` in its constructor to let cspot know to disable the software volume adjustment. Properly implementing external volume control (for example via dedicated hardware) will result in better playback quality since all the dynamic range is used to encode the samples.

The embedding program should also handle caching the authentication data, so that the user does not have to authenticate via the local network (Zeroconf) each time cspot is started. For reference on how to do it please refer to the `cspot-cli` target (It stores the data in `authBlob.json`). 

## Internal details

The connection with Spotify servers to play music and receive control information is pretty complex. First of all an access point address must be fetched from Spotify ([`ApResolve`](src/ApResolve.cpp) fetches the list from http://apresolve.spotify.com/). Then a [`PlainConnection`](include/PlainConnection.h) with the selected Spotify access point must be established. It is then upgraded to an encrypted [`ShannonConnection`](include/ShannonConnection.h).
