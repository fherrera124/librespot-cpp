# Estructura de `cspot`/`bell`

Documento de referencia — no de hallazgos/bugs (para eso está
[`spotify_component_analysis.md`](spotify_component_analysis.md)) — con
un mapa a grandes rasgos de qué vive dónde dentro de
`https://github.com/fherrera124/librespot-cpp/blob/master/`, para orientarse rápido en el código sin tener
que releer todo cada vez.

**Desactualizado (movido acá desde `cspot/docs/` el 2026-07-21, sin
reescribir el contenido)**: describe una etapa anterior a la migración
WS Dealer completa - todavía menciona `SpircHandler`/`PlaybackState`
como el motor activo, no menciona `PlayerEngine`/`PlaybackController`/
`TrackLoader`/`Decoder` (la interfaz Vorbis/MP3) ni la separación
`SpotifyConnectReceiver`. Sirve como mapa general de la idea de dos
capas (`cspot`/`bell`) y de `bell/` (que cambió menos), pero la tabla de
`src/`/`include/` y el diagrama de flujo al final están desactualizados.
Reescribir es trabajo pendiente, no cubierto por este movimiento de
archivos.

## La idea general: dos capas

`https://github.com/fherrera124/librespot-cpp/blob/master/` contiene en realidad **dos librerías
distintas**, ambas de [feelfreelinux](https://github.com/feelfreelinux),
vendorizadas juntas:

- **`cspot`** (`src/`, `include/`, `protobuf/`, en la raíz) — el
  protocolo de Spotify Connect en sí: emparejamiento, autenticación,
  Mercury (pub/sub sobre el que corren la mayoría de los mensajes),
  SPIRC (control remoto de reproducción), y la lógica de cola/descarga de
  pistas. Es código bastante agnóstico de plataforma.
- **`bell`** (`bell/`, un subdirectorio) — la capa de plataforma/HAL de
  la que `cspot` depende para todo lo que sí es específico del SO/
  hardware: tareas, sockets, TLS, salida de audio, JSON, logging.

`cspot` no sabe nada de FreeRTOS, sockets BSD ni mbedTLS directamente —
todo eso lo pide a través de las abstracciones que provee `bell`
(`bell::Task`, `bell::Socket`/`TLSSocket`, `Crypto`, etc.). Por eso,
cuando mbedTLS 4.0 rompió la capa criptográfica (sección 2 del documento
de análisis), lo que hubo que portar vivía en `bell/`, no en `cspot/`.

## `cspot/` (el protocolo)

### `src/` + `include/` — un archivo `.h`/`.cpp` por responsabilidad

| Archivo | Qué hace |
|---|---|
| `PlainConnection` | Socket TCP crudo hacia el Access Point de Spotify — framing de paquetes (`recvPacket`/`sendPrefixPacket`), antes de que exista cualquier cifrado. Acá vive el fix de F17/F28/F34. |
| `Shannon` | Implementación del cifrado de flujo Shannon — el cifrado simétrico que protege la conexión una vez completado el handshake Diffie-Hellman. |
| `ShannonConnection` | Envuelve `PlainConnection` aplicando `Shannon` — a partir de acá los paquetes van cifrados. Es lo que usa `MercurySession` como su conexión real (`shanConn`). |
| `Session` | Orquesta el handshake completo: `connectWithRandomAp()` (resuelve un AP vía `ApResolve` y abre `PlainConnection`), intercambio Diffie-Hellman + `AuthChallenges` (APHello/ClientResponse), y `authenticate()` (arma el paquete de login con `LoginBlob`). Es el punto de entrada que usa `cspot_connect.cpp`. |
| `AuthChallenges` | El intercambio criptográfico de bajo nivel del handshake: resuelve el DH con la respuesta del AP (`solveApHello`), arma el paquete de autenticación (`prepareAuthPacket`) que `Session` manda. Acá viven los fixes de F21/F23. |
| `LoginBlob` | Todo lo relacionado con credenciales: decodificación del blob de emparejamiento ZeroConf (`decodeBlob`/`decodeBlobSecondary`, cifrado con la shared key DH), y arma el JSON que expone `cspot_connect.cpp` en `/spotify_info`. Acá viven los fixes de F22/F35. |
| `MercurySession` | El protocolo Mercury propiamente dicho — pub/sub sobre la conexión Shannon, con su propia tarea (`bell::Task`, "mercury_dispatcher") despachando paquetes entrantes y manejando reconexión. La mayoría de los mensajes de Spotify (metadata, tokens, país, etc.) viajan como "Mercury requests" `SEND`/`SUB`/`SUBRES`. Acá viven los fixes de F19/F32. |
| `AccessKeyFetcher` | Pide/renueva el access token OAuth (`accounts.spotify.com/api/token`) que hace falta para las llamadas HTTPS (CDN, etc.) — usa `CONFIG_CSPOT_CLIENT_ID`/`SECRET`. Acá vive el fix de F26. |
| `ApResolve` | Resuelve la dirección del Access Point de Spotify a usar (`apresolve.spotify.com`). Acá vive el fix de F36. |
| `SpircHandler` | El protocolo SPIRC — control remoto: qué pista suena, play/pausa/skip/volumen, sincronización de estado con la app de Spotify. Es lo que dispara los `cspot_event_t` que ve `cspot_connect.cpp`. Acá vive el fix de F24. |
| `TrackQueue` | La cola de reproducción: qué pistas vienen, precarga (`queueNextTrack`), pedido de claves de audio. Corre en su propia tarea ("CSpotTrackQueue"). Acá viven los fixes de F25/F26. |
| `TrackPlayer` | Consume la cola (`TrackQueue`) y entrega PCM decodificado vía el callback que registra `cspot_connect.cpp` (`setDataCallback` → `pcmWrite`). |
| `CDNAudioFile` | Descarga y desencripta (AES-CTR) el audio real desde el CDN de Spotify una vez que `TrackQueue` consiguió la URL + clave. Acá vive el fix de F27. |
| `PlaybackState` | Estado de reproducción que se sincroniza en los frames SPIRC (posición, índice de pista actual, etc.). |
| `TrackReference` | Referencia liviana a una pista (URI/gid) dentro de la cola remota, antes de resolverse a metadata completa. |
| `TimeProvider` | Sincroniza el reloj con el servidor de Spotify (necesario para expiración de tokens, PING/PONG de `MercurySession`). |
| `Utils` | Funciones sueltas de bajo nivel: `extract`/`pack` (lectura/escritura de enteros big-endian de un `vector<uint8_t>`), `hton64`, hex, etc. — usadas por casi todo lo demás. |

### `protobuf/` — los esquemas del protocolo

`authentication.proto`, `keyexchange.proto`, `login5.proto`,
`mercury.proto`, `metadata.proto`, `spirc.proto` — se compilan a código
C vía `nanopb` (`bell/external/nanopb`) durante el build (ver hallazgo
F3 para el setup de `protoc` que hace falta). `authentication.options`
(junto a `authentication.proto`) es lo que fija el tamaño máximo de
`auth_data` en 512 bytes — el límite que F21 tuvo que empezar a
respetar.

## `bell/` (la capa de plataforma)

| Carpeta | Qué hay |
|---|---|
| `main/io/` | Sockets y red: `Socket`/`TCPSocket`/`TLSSocket` (acá vive nuestro port a mbedTLS 4.0, F29/F30), `HTTPClient`, `SocketStream`, `BellHTTPServer`. |
| `main/utilities/` | `BellTask` (wrapper de tareas FreeRTOS que usa *todo* `cspot`, incluido `CSpotConnectPlayer`), `Crypto` (nuestro port a mbedTLS 4.0/PSA, F37/F46), logging (`BellLogger`), `WrappedSemaphore`, colas, buffers circulares. |
| `main/audio-codec/` | Decoders — `VorbisDecoder` (envuelve `tremor`), y AAC/MP3/Opus que dejamos deshabilitados (`BELL_CODEC_* OFF` salvo Vorbis). |
| `main/audio-sinks/` | Salidas de audio. `BufferedAudioSink` (base compartida: dueña del periférico I2S + bell::CircularBuffer + tarea `i2sFeed`) y `PlainI2SAudioSink` (DAC I2S plano, sin códec I2C) están portados a `driver/i2s_std.h` (F51) — son las que usa `cspot_connect.cpp`. El resto (`AC101`, `ES8311`, `ES8388`, `ES9018`, `InternalAudioSink`, `SPDIF`, `TAS5711` — todas con códec/control por I2C) siguen en el driver I2S legacy y quedan excluidas del build (`bell/CMakeLists.txt` las filtra por `list(FILTER)`); portarlas es trabajo futuro si hace falta un DAC con códec real. |
| `main/audio-containers/`, `main/audio-dsp/` | Parseo de contenedores de audio (Ogg) y procesamiento de señal (resampling, etc.). |
| `main/platform/` | Código específico por SO — `esp/`, `linux/`, `apple/`, `win32/` — seleccionado vía `#ifdef ESP_PLATFORM` y equivalentes. `MDNSService`/`WrappedSemaphore` tienen su implementación acá para cada plataforma. |
| `main/asm/` | Algo de assembly puntual (biquad filters). |
| `external/` | Terceros vendorizados: `tremor` (decoder Vorbis entero, ver F1c), `fmt`, `nanopb` (codegen de protobuf), `cJSON`/`nlohmann_json` (usamos cJSON, `BELL_ONLY_CJSON ON`), y deshabilitados: `opus`, `opencore-aacdec`, `libhelix-mp3`, `civetweb`, `mqtt`, `portaudio`. |

## Dónde vive nuestro propio código

- **`components/cspot/cspot_connect.cpp`** (fuera de `cspot/`, en la raíz
  del componente) — la capa de adaptación propia: arranca `Session`,
  conecta los callbacks de `SpircHandler`/`TrackPlayer` con
  `PlainI2SAudioSink` (de `bell/`, ver abajo), expone el HTTP server de
  emparejamiento ZeroConf. Es lo único que escribimos desde cero para
  este proyecto — ya no hay un sink propio aparte, ver F51.
- **Dentro de `external/bell/`, archivos editados directamente** (ya no hay
  `vendor/`/`mbedtls4_compat/` — ver sección 0 del documento de
  análisis): `bell/main/utilities/Crypto.{h,cpp}`,
  `bell/main/io/TLSSocket.{h,cpp}`, `bell/main/io/X509Bundle.cpp`,
  `bell/main/audio-sinks/include/esp/BufferedAudioSink.h` +
  `bell/main/audio-sinks/esp/BufferedAudioSink.cpp`,
  `bell/main/audio-sinks/include/esp/PlainI2SAudioSink.h` +
  `bell/main/audio-sinks/esp/PlainI2SAudioSink.cpp` (F51).
- **Dentro de `src/`, archivos editados directamente**:
  `PlainConnection.cpp`, `MercurySession.cpp`, y los fixes de la sección
  6 del documento de análisis (`AuthChallenges.cpp`, `LoginBlob.cpp`,
  `AccessKeyFetcher.cpp`, `ApResolve.cpp`, `SpircHandler.cpp`,
  `TrackQueue.cpp`, `CDNAudioFile.cpp`).

Todo lo demás (el resto de `cspot/` y `bell/`, incluidos los sinks de
audio con códec I2C que no se portaron) es código de upstream sin tocar.

## Flujo a muy grandes rasgos

```
cspot_connect_start()
  -> arranca HTTP server + mDNS (emparejamiento ZeroConf)
  -> espera POST a /spotify_info -> LoginBlob::loadZeroconfQuery()
  -> Session::connectWithRandomAp()   (ApResolve -> PlainConnection)
  -> handshake DH + AuthChallenges    (PlainConnection, sin cifrar)
  -> Session::authenticate()          (LoginBlob -> AuthChallenges)
  -> ShannonConnection                (a partir de acá, cifrado)
  -> MercurySession (tarea propia)    (pub/sub: metadata, tokens, país...)
  -> SpircHandler                     (control remoto: play/pause/skip/volumen)
  -> TrackQueue (tarea propia) -> CDNAudioFile -> TrackPlayer
  -> pcmWrite() -> AudioSink::feedPCMFrames() (PlainI2SAudioSink)
  -> CircularBuffer -> tarea i2sFeed -> i2s_channel_write() -> parlante
```
