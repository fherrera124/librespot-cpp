# eSDK emulation — Spotify Connect receiver from the real Spotify eSDK

A self-contained, runnable harness that turns the **real Spotify embedded SDK**
(`libspotify_embedded_shared.so`) into a working Spotify Connect receiver: it
advertises itself over mDNS, accepts a login from the phone app (ZeroConf), and
— for a non-gated account — connects to `ap.spotify.com`, decrypts+decodes the
stream inside the eSDK, and emits PCM.

This preserves the investigation from 2026-09: whether a partner appkey +
partner device identity could obtain the legacy audio key for a **new** Spotify
account. **Answer: no.** See "What we learned" below.

## What's inside

```
src/esdk_server.c   the harness (the "main"): ZeroConf HTTP server + eSDK wiring
lib/
  esdk-1.20.0-armhf-v7.so   eSDK 1.20.0, 32-bit ARMv7 hard-float (from the Rocki firmware family)
  esdk-1.18.0-armel-v6.so   eSDK 1.18.0, ARMv6 soft-float (for Pi Zero / Pi 1)
  spotify_appkey.key        Rocki partner appkey (321 bytes)
Makefile            native build
run.sh              advertise over mDNS + run the server
```

Binaries come from the public `librespot-org/spotify-connect-resources` repo
(Rocki firmware). SHA-256:

```
5b93489d38e4c8041a39c0d40c05c958158795543d45c0167b8aeb55a996dc8d  esdk-1.20.0-armhf-v7.so
4cf20b8163125ecb57b1b43b7e548c66aba361dcf0c50becd7c169dd6d579630  esdk-1.18.0-armel-v6.so
3a16914b760586933f8873da9cabf019f209fa11bee8075cdaa72746ecf5beaa  spotify_appkey.key
```

## Requirements

The eSDK `.so` is `dlopen`'d at runtime, so the host must match its arch:

- **Raspberry Pi 2/3/4/5** with a 32-bit armhf userland → runs `esdk-1.20.0-armhf-v7.so` natively.
- **Pi Zero / Pi 1** (ARMv6) → use `esdk-1.18.0-armel-v6.so`.
- **x86 dev PC** → run it under `qemu-arm` (see bottom).

Also needs `avahi-publish` (package `avahi-utils`) for mDNS.

## Build & run (native, on the Pi)

```sh
make
./run.sh "cspot-esdk-test" 8000
```

Then open Spotify on a phone on the same LAN, pick **cspot-esdk-test** in the
device list, and hit play. Watch the log:

- Granted account → `SpConnectionLoginZeroConf -> 0`, then
  `*** FIRST AUDIO DATA ... KEY GRANTED` and `audio flowing: total frames=...`.
- Gated account → login still `-> 0`, but `Requesting key...` loops forever and
  no audio is ever delivered (this is the per-account server gate).

### Hearing it (granted account)

`onAudioData` gives interleaved S16 PCM. Pipe it to ALSA:

```sh
make
ESDK_PCM_STDOUT=1 ./esdk_server lib/esdk-1.20.0-armhf-v7.so lib/spotify_appkey.key \
    4 8000 "cspot-esdk-test" 2>run.log | aplay -f cd
# (start avahi-publish separately, or add it to run.sh)
```

`aplay -f cd` = 44100 Hz / 16-bit / stereo, which matches Spotify's OGG Vorbis.
The real sample rate/channels are logged from the `sp_sampleformat_t` the eSDK
passes; adjust `aplay` if they differ.

## The one non-obvious detail: `deviceID`

The getInfo response must advertise `deviceID = uniqueid_hash` (the 40-hex value
`SpZeroConfGetVars` returns, `= SHA1(uniqueid)`), **not** a MAC or the raw
uniqueid. The eSDK decrypts the inner credentials blob with a key derived from
`SHA1(device_id)`, using its internal device id (the uniqueid_hash). If the
advertised `deviceID` doesn't match, the phone fetches a blob bound to a
different id, the AES gives garbage, and you get `Parsing ZeroConf blob failed`
(-2/-3) and login returns 1. This was the fix that made login work.

The audio pipeline is a black box: the eSDK does CDN fetch → audio key (0x0C) →
AES decrypt → Vorbis decode internally and only ever hands out PCM via
`onAudioData`. The encrypted OGG and the key are never exposed.

## What we learned (why this is a museum piece, not a product)

The legacy audio-key gate (`0x0C` → error `0x0E`) is **server-side and
per-account**. Proven by control: same harness, same eSDK, same Rocki appkey,
same code — a working account plays, a gated new account is silent. The real
Sangean WFR-28C (eSDK 1.5.10) does the same: connects, phone shows "playing",
speaker stays silent. Confirmed externally by the eSDK changelog (v3.201.417:
*"Removed support for legacy streaming from Spotify's proprietary access
point"*), Spotify community reports, and librespot issue #1649.

So:

- **No client-side change** (identity, product enum, appkey, emulated eSDK)
  unlocks the legacy key for a gated account.
- This is **not portable to cspot/ESP32** — the eSDK is a compiled ARM Linux
  binary; it only emits PCM, it doesn't expose the key mechanism.
- For a non-gated account this is a functional Connect receiver, but
  **librespot / Snapcast (Soloist)** already do that better and open-source.

Kept as a reference: the working eSDK harness, the v4 SpConfig layout, and the
deviceID fix.

## Running on an x86 dev PC (qemu)

```sh
# static qemu-arm + an armhf glibc sysroot (e.g. Bootlin armv7-eabihf)
arm-linux-gcc --sysroot="$SYSROOT" -O2 -o esdk_server src/esdk_server.c -ldl -lm
qemu-arm-static -L "$SYSROOT" -E LD_LIBRARY_PATH="$SYSROOT/lib:$PWD/lib" \
    ./esdk_server lib/esdk-1.20.0-armhf-v7.so lib/spotify_appkey.key 4 8000 "cspot-esdk-test"
# advertise separately:  avahi-publish -s "cspot-esdk-test" _spotify-connect._tcp 8000 VERSION=1.0 CPath=/
```
