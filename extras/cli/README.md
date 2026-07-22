# cspotcli

A command-line Spotify Connect speaker for Linux, built on top of
`cspot::SpotifyConnectReceiver` - the same class the ESP32 app
(`cspot_connect.cpp`, in the `cspot` repo) uses. It advertises itself via
mDNS/ZeroConf; select it from the Spotify app to start playback.

## Prerequisites (Ubuntu/Debian)

```bash
sudo apt install build-essential cmake libmbedtls-dev \
    libavahi-client-dev libavahi-common-dev \
    libasound2-dev portaudio19-dev
```

Also needs a Python 3 with the `protobuf` module available on `PATH`
(used only at build time, to generate nanopb code from the `.proto`
files) - if `python3 -c "import google.protobuf"` fails, either
`pip install protobuf` or create a venv with it and prepend its `bin/`
to `PATH` before building.

You'll also need a Spotify Developer Dashboard app (client ID + secret)
- see below.

## Building

From the repo root:

```bash
mkdir build && cd build
cmake -DCSPOT_TARGET_CLI=ON -DBELL_SINK_ALSA=ON ..   # or -DBELL_SINK_PORTAUDIO=ON
cmake --build . -j$(nproc)
```

`BELL_SINK_ALSA`/`BELL_SINK_PORTAUDIO` are `bell`'s own options (see
`external/bell/CMakeLists.txt`) - set one of them here, not inside
`extras/cli/`, since `bell` is configured before this directory is
reached. Without either, `cspotcli` falls back to a `NamedPipeAudioSink`
(writes raw PCM to a named pipe instead of playing it).

## Running

```bash
./extras/cli/cspotcli --client-id <id> --client-secret <secret>
```

`--client-id`/`--client-secret` are required - create an app at the
[Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
to get them (used for the `client_credentials` grant that fetches track
audio from the CDN, not for login - pairing itself is ZeroConf-only, no
username/password support). `--device-name <name>` sets what's shown in
the Spotify app (default `"CSpot CLI"`); `-b/--bitrate <96|160|320>`
sets the streaming bitrate. `--help` lists all options.

Make sure `avahi-daemon` is running (`systemctl status avahi-daemon`) -
`cspotcli` won't be discoverable by the Spotify app otherwise. Once
running, open the Spotify app on the same network and select the
device by name; Ctrl+C stops it cleanly.
