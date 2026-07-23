# Estructura de `cspot`/`bell`

Documento de referencia — no de hallazgos/bugs (para eso está
[`spotify_component_analysis.md`](spotify_component_analysis.md), y para
la migración a Dealer WebSocket
[`dealer_websocket_migration.md`](dealer_websocket_migration.md)) — con
un mapa a grandes rasgos de qué vive dónde dentro de
`https://github.com/fherrera124/librespot-cpp`, para orientarse rápido
en el código sin tener que releer todo cada vez.

**Reescrito 2026-07-21** (la versión anterior describía la etapa
pre-Dealer, con `SpircHandler`/`PlaybackState` como motor activo - ver
historial de git si hace falta esa versión vieja).

## La idea general: dos capas

El repo contiene en realidad **dos librerías distintas**, vendorizadas
juntas (fork de [feelfreelinux/cspot](https://github.com/feelfreelinux/cspot)):

- **`cspot`** (`src/`, `include/`, `protobuf/`, en la raíz) — el
  protocolo de Spotify Connect en sí: emparejamiento, autenticación,
  Mercury (pub/sub), Dealer WebSocket + connect-state (reemplazo
  completo de SPIRC, ver `docs/dealer_websocket_migration.md`), y la
  lógica de cola/carga/descarga/decodificación de pistas. Código
  bastante agnóstico de plataforma.
- **`bell`** (`external/bell/`) — la capa de plataforma/HAL de la que
  `cspot` depende para todo lo específico del SO/hardware: tareas,
  sockets, TLS, salida de audio, JSON, logging.

`cspot` no sabe nada de FreeRTOS, sockets BSD ni mbedTLS directamente —
todo eso lo pide a través de las abstracciones que provee `bell`
(`bell::Task`, `bell::Socket`/`TLSSocket`, `Crypto`, etc.). Por eso,
cuando mbedTLS 4.0 rompió la capa criptográfica, lo que hubo que portar
vivía en `bell/`, no en `cspot/` (sección 2 de
`spotify_component_analysis.md`).

## `cspot/` (el protocolo)

### Sesión, autenticación, transporte crudo

| Archivo | Qué hace |
|---|---|
| `PlainConnection` | Socket TCP crudo hacia el Access Point de Spotify — framing de paquetes, antes de que exista cualquier cifrado. |
| `Shannon` | Implementación del cifrado de flujo Shannon — el cifrado simétrico que protege la conexión una vez completado el handshake Diffie-Hellman. |
| `ShannonConnection` | Envuelve `PlainConnection` aplicando `Shannon` — a partir de acá los paquetes van cifrados. Es lo que usa `MercurySession` como su conexión real (`shanConn`). |
| `Session` | Orquesta el handshake completo: `connectWithRandomAp()`, Diffie-Hellman + `AuthChallenges`, y `authenticate()` (`LoginBlob`). Punto de entrada que usa `SpotifyConnectReceiver`. |
| `AuthChallenges` | El intercambio criptográfico de bajo nivel del handshake. |
| `LoginBlob` | Credenciales: decodificación del blob de emparejamiento ZeroConf, arma el JSON que expone el HTTP server de pairing. |
| `ApResolve` | Resuelve las direcciones de Access Point/Dealer/spclient a usar. |
| `TimeProvider` | Sincroniza el reloj con el servidor de Spotify (expiración de tokens, PING/PONG). |
| `Login5Client` | Token de Spotify's Login5 API — el que usa `DealerSession` para autenticar la conexión WebSocket al Dealer (`wss://.../?access_token=...`). Distinto del token OAuth clásico que pide `AccessKeyFetcher`. |
| `Utils` | Funciones sueltas de bajo nivel: `extract`/`pack`, `hton64`, hex, etc. |

### Mercury (pub/sub sobre la conexión Shannon)

| Archivo | Qué hace |
|---|---|
| `MercurySession` | El protocolo Mercury propiamente dicho — su propia tarea (`bell::Task`, "mercury_dispatcher") despachando paquetes entrantes y manejando reconexión. Hoy solo lo usan las metadata GETs de `TrackLoader` y el resto de la sesión AP (auth, audio keys, país, sincronización de reloj) - toda la maquinaria SUB/UNSUB/SUBRES de SPIRC se retiró con el Dealer (`dealer_websocket_migration.md` §12). |
| `AccessKeyFetcher` | Pide/renueva el access token OAuth (`accounts.spotify.com/api/token`) que hace falta para las llamadas HTTPS (CDN, etc.). |

### Motor de reproducción (cola, carga, decodificación)

| Archivo | Qué hace |
|---|---|
| `TrackQueue` | Dueña de la cola de reproducción (`preloadedTracks`) y los 8 métodos de dominio (`skipTrack`/`updateTracks`/`insertNext`/etc.) - mutación instantánea, en memoria. Ya no hereda `bell::Task` - la carga de red vive en `TrackLoader`. |
| `TrackLoader` | Motor de red: avanza la máquina de estados de cada `QueuedTrack` (metadata → audio key → CDN url) en su propia tarea. Nunca toca `preloadedTracks` directo - se comunica con `TrackQueue` vía dos callbacks inyectados (snapshot/top-up). |
| `TrackPlayer` | Consume la cola, decodifica (vía `Decoder`) y alimenta PCM al `AudioSink` en su propia tarea. |
| `Decoder` / `VorbisTrackDecoder` / `Mp3TrackDecoder` | Interfaz polimórfica de decodificación (extraída de `TrackPlayer` para separar Vorbis/MP3 por OCP) - cada uno envuelve su codec (`tremor`/`libhelix-mp3` en `bell/external/`). |
| `CDNAudioFile` | Descarga y desencripta (AES-CTR) el audio real desde el CDN de Spotify una vez que `TrackLoader` consiguió la URL + clave. |
| `TrackReference` | Referencia liviana a una pista (URI/gid) dentro de la cola, antes de resolverse a metadata completa. |
| `ContextResolver` | Resuelve un context URI (playlist/álbum/etc., de un `transfer`/`play`) a la lista de `TrackReference` correspondiente. |

### Dealer WebSocket + Connect-State (el reemplazo completo de SPIRC)

Ver `docs/dealer_websocket_migration.md` para el detalle de protocolo -
acá solo el mapa de clases.

| Archivo | Qué hace |
|---|---|
| `DealerSession` | Dueño de la conexión WebSocket al Dealer y su loop de recepción - parsea el envelope JSON y despacha por URI/tipo. Tiene su propia tarea nested `CommandWorker` que ejecuta `player/command` requests fuera del loop de recepción (para no trabarlo). |
| `PlayerEngine` | Publica el connect-state del dispositivo a spclient (tarea propia de PUT) y es dueño del motor de reproducción completo (`PlaybackController`/`PlayerStateModel`/`PlayerCommandHandler`). Antes se llamaba `ConnectStateHandler`. |
| `PlaybackController` | Dueño de `TrackQueue`/`TrackPlayer`, tracking de posición, y la superficie de control (load/skip/seek/pause). |
| `PlayerCommandHandler` | Decodifica y ejecuta los requests `hm://connect-state/v1/player/command` (transfer/play/pause/skip/seek/repeat/edición de cola) contra `PlaybackController`/`PlayerStateModel`/`ContextResolver`. |
| `PlayerStateModel` | Dueño del `PlayerState` protobuf + session_id/playback_id/context_uri/restrictions/context_metadata que se mandan en cada PUT. Antes se llamaba `ConnectStateModel`. |
| `PutStateClient` | Transporte HTTP PUT hacia spclient - resolución de host/reuso de conexión/retry/rate-limit. |

### Ciclo de vida del dispositivo

| Archivo | Qué hace |
|---|---|
| `SpotifyConnectReceiver` | "Corre un dispositivo Spotify Connect de punta a punta": pairing ZeroConf (HTTP server + mDNS), conexión/auth/retry de sesión, y el motor completo (vía `DealerSession`/`PlayerEngine`) - todo lo que un consumidor en cualquier plataforma necesita, dado solo un `AudioSink` y config. Es lo que instancia `cspot_connect.cpp` (ver abajo) - antes esa lógica de ciclo de vida vivía ahí mismo. |

### Soporte/utilidades

`CSpotContext` (el `Context` compartido - config, session, timeProvider -
enhebrado por casi todas las clases de arriba), `ConstantParameters`,
`CspotAssert`, `HttpRetry`, `Packet`.

### `protobuf/` — los esquemas del protocolo

`authentication.proto`, `keyexchange.proto`, `login5.proto`,
`mercury.proto`, `metadata.proto`, `connectstate.proto`,
`clienttoken.proto` — se compilan a código C vía `nanopb`
(`external/bell/external/nanopb`) durante el build. `spirc.proto` se
retiró junto con `SpircHandler`.

## `bell/` (la capa de plataforma)

| Carpeta | Qué hay |
|---|---|
| `main/io/` | Sockets y red: `Socket`/`TCPSocket`/`TLSSocket` (port a mbedTLS 4.0), `HTTPClient`, `SocketStream`, `WebSocketTransport` (RFC 6455, usado por `DealerSession`), `SimpleHTTPServer` (HTTP mínimo portable, usado por el pairing ZeroConf). |
| `main/utilities/` | `BellTask` (wrapper de tareas FreeRTOS/pthread que usa *todo* `cspot` - ver `stopAndWait()`/`onStopRequested()`/`shouldStop()`, 2026-07-21), `Crypto` (port a mbedTLS 4.0/PSA), logging (`BellLogger`), `WrappedSemaphore`, colas, buffers circulares. |
| `main/audio-codec/` | Decoders — envuelven `tremor` (Vorbis)/`libhelix-mp3` (MP3); AAC/Opus deshabilitados (`BELL_CODEC_* OFF`). |
| `main/audio-sinks/` | Salidas de audio. `BufferedAudioSink`/`PlainI2SAudioSink` (DAC I2S plano, sin códec I2C) portadas a `driver/i2s_std.h` - son las que usa `cspot_connect.cpp`. El resto (`AC101`, `ES8311`, `ES8388`, `ES9018`, `InternalAudioSink`, `SPDIF`, `TAS5711` - con códec/control I2C) siguen en el driver I2S legacy y quedan excluidas del build (`bell/CMakeLists.txt` las filtra). |
| `main/audio-containers/`, `main/audio-dsp/` | Parseo de contenedores de audio (Ogg) y procesamiento de señal. |
| `main/platform/` | Código específico por SO — `esp/`, `linux/`, `apple/`, `win32/`. |
| `main/asm/` | Algo de assembly puntual (biquad filters). |
| `external/` | Terceros vendorizados: `tremor`, `fmt`, `nanopb`, `cJSON`/`nlohmann_json` (usamos cJSON, `BELL_ONLY_CJSON ON`), y deshabilitados: `opus`, `opencore-aacdec`, `libhelix-mp3` (este sí en uso, ver audio-codec), `civetweb`, `mqtt`, `portaudio`. |

## Dónde vive nuestro propio código

El componente `cspot` (`spotify-jc3248w535`, repo aparte) ya no arma el
ciclo de vida del dispositivo él mismo - eso lo hace por completo
`SpotifyConnectReceiver` (arriba). Lo único que le queda a
`components/cspot/cspot_connect.cpp` es la capa de adaptación
específica de ESP32: construye el `PlainI2SAudioSink` real (pines I2S,
Kconfig), pasa client id/secret, y traduce los eventos del motor
(`cspot::Event`) a la API C de callbacks que consume la UI LVGL. Ver
`docs/spotify_component_analysis.md` para el detalle de qué se portó/
qué bugs se encontraron a nivel `cspot`/`bell` en general.

## Flujo a muy grandes rasgos

```
SpotifyConnectReceiver(audioSink, config, eventHandler, ...)
  -> arranca HTTP server + mDNS (emparejamiento ZeroConf)
  -> espera POST a /spotify_info -> LoginBlob::loadZeroconfQuery()
  -> Session::connectWithRandomAp()   (ApResolve -> PlainConnection)
  -> handshake DH + AuthChallenges    (PlainConnection, sin cifrar)
  -> Session::authenticate()          (LoginBlob -> AuthChallenges)
  -> ShannonConnection                (a partir de acá, cifrado)
  -> DealerSession (tarea propia)      (Login5Client -> token -> wss:// dealer)
       -> CommandWorker (tarea propia) ejecuta player/command requests
       -> PlayerEngine: cluster/volume pushes, PUT de connect-state (tarea propia)
       -> PlaybackController::loadTracks()/skipTrack()/...
  -> TrackQueue (mutación instantánea) + TrackLoader (tarea propia, red)
  -> TrackPlayer (tarea propia) -> Decoder (Vorbis/MP3) -> AudioSink::feedPCMFrames()
  -> CircularBuffer -> tarea i2sFeed -> i2s_channel_write() -> parlante
```
