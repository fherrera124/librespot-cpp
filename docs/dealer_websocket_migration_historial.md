# Migración a WebSocket Dealer + Connect-State — análisis, hallazgos, desafíos y plan

Rama: `feature/ws-dealer` (creada desde `main`). Este documento es el
punto de partida de esa rama: qué averiguamos investigando librespot
(Rust) y librespot-go, qué de eso aplica a `cspot`, qué hay que
construir de cero, y un plan de implementación por fases. No es un
documento de hallazgos F-number (no es un bug ni una decisión ya
aplicada) — es un documento de diseño, vivo mientras dure el trabajo en
esta rama.

## 0. Resumen ejecutivo — respuesta directa a la pregunta central

**¿Siguen usando SPIRC mutuamente (junto con Dealer)? No.** Verificado
de forma independiente en dos codebases (librespot Rust, rama `dev`, y
`devgianlu/go-librespot`, el fork activo de librespot-go — ver §1):
**SPIRC está completamente muerto en ambas.** No es que coexista con
Dealer — el protocolo SPIRC (frames protobuf `Frame` publicados/
suscritos vía Mercury en `hm://remote/user/.../v23`, exactamente lo que
`SpircHandler::subscribeToMercury()` hace hoy en este proyecto) tiene
**cero referencias** en ninguno de los dos repos. En Rust,
`protocol/proto/spirc.proto` todavía existe en el árbol pero **no se
compila** (`protocol/build.rs` ya no lo lista) — código muerto dejado
como referencia histórica, nada más.

Lo que reemplaza a SPIRC es una arquitectura de dos mitades, no un
único mecanismo:

| Antes (SPIRC/Mercury) | Ahora |
|---|---|
| Recepción de comandos remotos (play/pause/seek/next/etc.) | **Dealer WebSocket** (push JSON, entrante) |
| Publicación del propio estado de reproducción | **PUT HTTP a `spclient`** con protobuf `PutStateRequest` (saliente, **no** va por el WebSocket) |
| Mercury pub/sub en `hm://remote/user/.../v23` | Eliminado por completo |

Mercury **no desaparece** — sigue vivo, pero reducido a un rol angosto
(ver §1.2): metadata genérica (track/album/artist) y un token OAuth
"legacy" (`hm://keymaster/token/...`) que resulta ser exactamente el
mecanismo que este proyecto necesita para autenticar el propio Dealer
(ver §4.1 — esto es un hallazgo importante, no solo curiosidad).

La conexión AP clásica (TCP + Shannon, el login ZeroConf que ya tenemos
funcionando y verificado con F93/F101) **sigue siendo necesaria** — para
el login en sí, para el fetch de audio keys, y para ese token Mercury.
No hay que tocarla.

---

## 1. Hallazgos: librespot (Rust)

Investigado contra la rama `dev` de `librespot-org/librespot` (fetch +
checkout completo del repo, no resúmenes de la UI de GitHub).

### 1.1 Arquitectura resultante

| Concern | Mecanismo |
|---|---|
| Login/handshake | AP TCP + cifrado Shannon (sin cambios respecto a lo que ya tenemos) |
| Token OAuth "legacy", metadata GETs | Mercury pub/sub, sobre la conexión AP |
| Audio key | Canal AP directo (paquetes `RequestKey`/`AesKey`), no Mercury, no Dealer |
| Audio (los bytes en sí) | CDN sobre HTTPS — igual que `CDNAudioFile.cpp` ya hace hoy |
| **Push de control remoto** (cluster updates, comandos, volumen, logout, cambios de playlist/sesión/atributos de usuario) | **Dealer WebSocket** (entrante) |
| **Publicar el propio estado de conexión** | **PUT HTTP** a `spclient`, `/connect-state/v1/devices/{device_id}`, **no** por el WebSocket |

`connect/src/spirc.rs` (1936 líneas) es una **reimplementación
completa**: conserva el nombre público (`Spirc`, `SpircCommand`) por
compatibilidad con quien lo llama, pero por dentro no toca Mercury para
nada — es 100% Dealer + connect-state. Este es un patrón arquitectónico
a replicar: **el nombre/forma de la API pública se mantiene estable, la
implementación interna se reemplaza entera.**

### 1.2 Conexión y autenticación del Dealer

1. **Descubrimiento** (apresolve): `GET
   https://apresolve.spotify.com/?type=accesspoint&type=dealer&type=spclient`
   → JSON `{"accesspoint": [...], "dealer": [...], "spclient": [...]}`,
   cada uno una lista de `"host:port"`. Fallback hardcodeado si falla:
   `dealer.spotify.com:443`.
2. **Token**: `session.login5().auth_token()` — un flow OAuth más nuevo
   ("Login5", HTTPS+protobuf a `https://login5.spotify.com/v3/login`),
   independiente del token Mercury "legacy" (ver más abajo, §4.1, por
   qué el legacy es el que nos interesa a nosotros, no Login5).
3. **URL**: `wss://{host}:{port}/?access_token={token}` — el token va
   como **query param**, no como header. La función que arma la URL se
   pasa como closure, no como URL ya resuelta, específicamente para que
   un reconnect pida un token fresco (comentario textual en el código:
   de lo contrario, al reconectar con la URL/token inicial ya vencido,
   solo se obtiene un 401).
4. **Handshake WS**: `tokio_tungstenite::client_async_tls` sobre TCP.
5. **Primer mensaje esperado, a nivel de aplicación**: un push (no un
   header HTTP) en `hm://pusher/v1/connections/`, cuyo
   `headers["Spotify-Connection-Id"]` da un ID de conexión. Ese ID se
   guarda y se adjunta como header `x-spotify-connection-id` en cada
   PUT posterior a `spclient` — así es como Spotify correlaciona el
   estado publicado por HTTP con el socket WS específico que lo generó.
6. **Keepalive**: ping WS de verdad (frame de control, no JSON) cada
   30s, timeout de pong 3s, reconexión a los 10s si falla.

### 1.3 Formato de mensaje

Frames de texto WebSocket conteniendo JSON. Unión discriminada por
`"type"`:

- **`"message"`** (push, fire-and-forget):
  `{headers, method?, payloads: [...], uri}`. `payloads[0]` es un
  string base64 (o bytes crudos, o JSON inline según el caso) que, tras
  decodificar, puede venir además comprimido con gzip si
  `headers["Transfer-Encoding"] == "gzip"` — y el resultado final es o
  bien protobuf crudo o bien JSON, **según la URI** (no hay un único
  formato de payload).
- **`"request"`** (RPC, espera respuesta):
  `{headers, message_ident, key, payload: {compressed: "<base64[+gzip]>"}}`.
  El body decodificado es JSON, con forma
  `{message_id, sent_by_device_id, command: {endpoint: "...", ...}}` —
  `endpoint` distingue `Transfer`, `Play`, `Pause`, `SeekTo`, `SkipNext`,
  `SkipPrev`, `Resume`, `SetShufflingContext`, `SetRepeatingTrack`,
  `SetRepeatingContext`, `AddToQueue`, `SetQueue`, `SetOptions`,
  `UpdateContext`, más un catch-all para forward-compat.
- **Respuesta a un `"request"`**: el cliente escribe de vuelta
  `{"type":"reply","key":"<el mismo key>","payload":{"success":true|false}}`.
  Dato interesante: si un handler nunca responde explícitamente, hay un
  `Drop` que manda `{"success":false}` como red de seguridad — nunca
  dejar una request colgada sin respuesta.
- **Ruteo**: por prefijo de URI (para `"message"`) o por
  `message_ident` exacto (para `"request"`), sobre una estructura tipo
  trie (`SubscriberMap`/`HandlerMap`).

### 1.4 Tabla de URIs → acción concreta

| URI / message_ident | Tipo | Qué hace |
|---|---|---|
| `hm://pusher/v1/connections/` | message | Entrega el `Spotify-Connection-Id` |
| `hm://connect-state/v1/cluster` | message (protobuf) | "Cluster update" — otro dispositivo tomó control / cambió el estado en otro lado |
| `hm://connect-state/v1/connect/volume` | message (protobuf) | Cambio de volumen remoto |
| `hm://connect-state/v1/connect/logout` | message (protobuf) | Logout — **en librespot, ni siquiera está implementado todavía** (`warn!` + `// todo`), dato útil: no es bloqueante tenerlo desde el día uno |
| `hm://playlist/v2/playlist/` | message | Modificación de playlist |
| `social-connect/v2/session_update` | message (JSON) | Actualización de sesión social |
| `spotify:user:attributes:update`/`:mutated` | message (protobuf) | Cambios de atributos de usuario |
| `hm://connect-state/v1/player/command` | **request** | Los comandos de control remoto reales — el único que exige respuesta JSON |

### 1.5 Librería WebSocket usada (Rust, solo como dato, no aplica directo a C++)

--eliminar seccion

---

## 2. Hallazgos: librespot-go

Primero hubo que identificar el repo correcto — hay varios forks con el
nombre "librespot-go":

| Repo | Estado |
|---|---|
| `librespot-org/librespot-golang` | 152 commits, sin releases, el propio README dice "highly experimental", **cero código de dealer/connect-state/websocket** — puerto pre-Dealer, abandonado |
| `kotahu/go-librespot` | Fork de abajo, no investigado directamente |
| **`devgianlu/go-librespot`** | 579 commits, v0.7.4 (2026-06-19), último commit 2026-07-07, 338 estrellas, CI activo — **este es el mantenido, el que se investigó** |

Clonado localmente (shallow, HEAD `8331ce9`) para leer código real, no
resúmenes de la UI de GitHub.

### 2.1 Confirmación: mismos hallazgos que Rust, independientemente

`grep -ril "spirc"` en todo el repo: **cero resultados.** Misma
arquitectura de dos mitades (Dealer entrante + PUT `connect-state`
saliente) que en Rust — confirmado por una implementación totalmente
distinta, lo cual es una señal fuerte de que este es el diseño
"correcto" a replicar, no una idiosincrasia de un solo proyecto.

Mercury sobrevive igual que en Rust: cliente RPC genérico
(`mercury/client.go`) para llamadas puntuales de metadata,
correlacionadas por secuencia en un `map[uint64]hermesRequest`, timeout
15s. No pub/sub, no comandos de reproducción.

### 2.2 Conexión/auth — mismo patrón, detalles concretos de Go

- Descubrimiento: mismo endpoint `apresolve.spotify.com`, cacheado con
  expiración de 1h por tipo.
- Token: `GetLogin5TokenFunc` — mismo concepto de token "Login5"
  compartido con las llamadas HTTP a `spclient`.
- URL: `wss://{dealer-host}/?access_token={token}` — igual que Rust,
  query param.
- Dial: solo `User-Agent` como header explícito.
- **Keepalive, diferencia notable respecto a Rust**: acá el ping es un
  **frame de texto JSON literal** `{"type":"ping"}`, no un frame de
  control WS — el servidor responde `{"type":"pong"}`. Intervalo 30s,
  timeout 40s (`pingInterval + timeout`). Esto sugiere que el backend de
  Spotify acepta ambas formas (control frame o JSON), no hay que
  asumir una sola.
- Reconexión con backoff exponencial (`cenkalti/backoff`); si se agota,
  cierra todos los canales de recepción y se rinde.

### 2.3 Estructura del mensaje (Go) — mismo formato, nombres de campo confirmados

```go
type RawMessage struct {
    Type         string            `json:"type"`          // "message" | "request" | "ping" | "pong"
    Method       string            `json:"method"`
    Uri          string            `json:"uri"`
    Headers      map[string]string `json:"headers"`
    MessageIdent string            `json:"message_ident"`
    Key          string            `json:"key"`
    Payloads     []interface{}     `json:"payloads"`
    Payload      struct {
        Compressed []byte `json:"compressed"`
    } `json:"payload"`
}
```

Confirma exactamente lo mismo que Rust encontró de forma independiente:
`"message"` ruteado por prefijo de URI, `"request"` ruteado por
`message_ident` exacto (y acá el código Go explícitamente **panickea**
si se intenta registrar el mismo `message_ident` dos veces — dato útil
de diseño: un handler por endpoint, no una lista).

### 2.4 El bucle completo, con nombres de archivo/línea reales

`daemon/player.go:714-715` se suscribe así:

```go
msgRecv := p.sess.Dealer().ReceiveMessage("hm://pusher/v1/connections/", "hm://connect-state/v1/")
reqRecv := p.sess.Dealer().ReceiveRequest("hm://connect-state/v1/player/command")
```

Y el flujo concreto confirmado leyendo el código:

1. Llega `hm://pusher/v1/connections/` (el `Spotify-Connection-Id` va
   en un **header** del mensaje, no en el payload).
2. Inmediatamente, el cliente hace su **primer** PUT de connect-state
   con motivo `PutStateReason_NEW_DEVICE`.
3. `hm://connect-state/v1/cluster` trae un `ClusterUpdate` protobuf —
   si indica que otro dispositivo pasó a estar activo y su timestamp es
   más nuevo que el último transfer local, este cliente deja de ser el
   dispositivo activo y para la reproducción.
4. El único `"request"` registrado
   (`hm://connect-state/v1/player/command`) se despacha por
   `Command.Endpoint` — `"transfer"` reconstruye cola/contexto, ajusta
   posición/pausa, carga el track, y responde `{"success":true/false}`.
5. **Todo cambio de estado termina llamando a `flushState()`**, que
   serializa `Device{DeviceInfo, PlayerState}` como
   `connectpb.PutStateRequest` y hace
   `PUT https://{spclient}/connect-state/v1/devices/{deviceId}` con
   header `X-Spotify-Connection-Id` = el ID capturado en el paso 1.
   Rate-limitado a un PUT cada 200ms, coalescendo ráfagas con un timer.

Esto es el mecanismo completo: **el ID de conexión que entrega el
WebSocket es la credencial que autoriza el PUT que le responde** — el
lazo que reemplaza al broadcast-por-Mercury de SPIRC.

### 2.5 Librería WebSocket (Go, dato de referencia)

--eliminar seccion

---

## 3. La librería WebSocket para el port a C++/ESP32

--eliminar seccion

### 3.1 Qué aprendimos del uso real en `spotify_app` (mecánica, no arquitectura)

--eliminar seccion

### 3.2 Por qué `spotify_app` no es la arquitectura a copiar (solo la mecánica)

`spotify_app` es un **cliente Web API + Dealer actuando como control
remoto** de una sesión Connect que corre en OTRO dispositivo — usa el
Dealer en modo **observador/controlador**: recibe `PLAYER_STATE_CHANGED`
vía un envelope `{"uri":"wss://event",...,"payloads":[{"events":[{"type":"PLAYER_STATE_CHANGED",...}]}]}`
(confirmado leyendo `parse_objects.c:parse_track()`), y envía comandos
como llamadas HTTP normales a la Web API (`player_cmd()`), no por el
WebSocket.

`cspot` necesitaría el rol **opuesto**: ser el **dispositivo**
(aparecer en el selector Connect, recibir comandos de control remoto
vía Dealer, publicar su propio estado vía PUT) — el mismo rol que
tienen librespot/librespot-go, con el envelope `"type":"message"`/
`"request"` + `hm://connect-state/v1/...` de §1-2, no el
`"uri":"wss://event"` que `spotify_app` consume. Son primos, no el
mismo protocolo — la parte reusable de `spotify_app` es la **mecánica
de `esp_websocket_client`** (§3.1), no su modelo de mensajes ni su
autenticación (Web API OAuth de usuario final, vía token Discord según
el propio proyecto — nada que ver con el login ZeroConf/AP de `cspot`).

### 3.3 Revisión: no es todo-o-nada — mismo patrón que `Crypto.cpp` (F98-F100)

Pregunta del usuario que disparó esto: "¿cómo se adaptaría el core,
para no depender de ESP32?", seguida de "¿perdemos alguna ventaja al
descartar `esp_websocket_client`?" — la segunda pregunta es la que
corrige el planteo: descartarlo del todo sí tiene un costo real, y no
hace falta pagarlo.

**Lo que se pierde si se descarta `esp_websocket_client` por completo**:
robustez ya probada contra el servidor Dealer real (`spotify_app` la
validó en este mismo hardware); varias esquinas del RFC 6455 que son
genuinamente difíciles de acertar a la primera.

**No hace falta elegir uno solo — este proyecto ya tiene el patrón
exacto para esto**: `Crypto.cpp` (F98-F100) usa `mbedtls_mpi_*` real,
acelerado por hardware, en la rama `#ifdef ESP_PLATFORM` — la mejor
opción posible en producción — y `BigUint` propio, portable, **solo en
host/test, nunca en producción**, en la rama `#else`. Nadie pierde nada
ahí: el ESP32 real sigue usando lo probado, el host gana testeabilidad
sin comprometer producción.

**Diseño propuesto, aplicando lo mismo acá**: una interfaz abstracta
`WebSocketTransport` (mismo espíritu que `bell::Socket`), con dos
implementaciones:

---

## 4. Qué de esto ya existe en `cspot` y se puede reusar

### 4.1 Token de sesión — el hallazgo más importante para acotar el trabajo

`cspot` ya tiene `AccessKeyFetcher` (`AccessKeyFetcher.h/.cpp`), pero
usa un grant **`client_credentials`** contra
`https://accounts.spotify.com/api/token` con `ctx->config.clientId`/
`clientSecret` — es un **token a nivel de aplicación**, sin ningún
usuario asociado (se usa hoy para CDN/storage-resolve). **Este token no
sirve para el Dealer** — el Dealer necesita un token atado a la cuenta
del usuario logueado, para que Spotify pueda asociar el dispositivo a
esa cuenta específica y que otros clientes de esa misma cuenta lo vean
en el selector.

Tanto Rust como Go usan un flow más nuevo ("Login5") para ese token de
usuario — **Rust tiene, además, un mecanismo viejo tipo Mercury
"legacy"**: `hm://keymaster/token/authenticated?scope=...&client_id=...&device_id=...`
(`core/src/token.rs:98`), una **request Mercury normal**
(`MercurySession::execute()`, no `executeSubscription()`) — exactamente
la misma máquina que `SpircHandler::subscribeToMercury()` y
`TrackQueue` ya usan hoy, verificada y estable.

**Corrección (2026-07-13), tras verificar quién llama a cada
mecanismo en el código real, no solo si existe**: el path keymaster
**no es el que usa el flujo real del Dealer** en ninguna de las dos
implementaciones. En Rust, el único llamador de
`TokenProvider::get_token()` en todo el repo es
`examples/get_token.rs` — una herramienta de diagnóstico standalone,
no el cliente real. La conexión Dealer de verdad
(`core/src/dealer/manager.rs:87`) usa
`session.login5().auth_token()`. En Go, `grep -rl keymaster` sobre
todo el repo da **cero resultados**. El mecanismo que de verdad
autentica el Dealer en ambos es Login5 (HTTPS+protobuf a
`login5.spotify.com/v3/login`), sensiblemente más grande: requiere un
`client_token` previo (otro endpoint/protobuf), un schema
`login5.proto` nuevo, y puede exigir resolver desafíos **hashcash**
(proof-of-work) antes de aceptar el login — lógica nueva de verdad, no
solo un request/response.

**Decisión (2026-07-13, confirmada con el usuario)**: no descartar
keymaster solo por esto — es una sonda barata (reusa infraestructura
Mercury existente, sin schema nuevo) que vale la pena probar contra el
Dealer real antes de pagar el costo de Login5. Si el Dealer rechaza un
token obtenido así, se sabe rápido y recién ahí se justifica Login5.
Fase 1 (más abajo, "Resultados") implementa esta sonda y deja
pendiente de hardware si el Dealer la acepta — eso no se puede saber
hasta la Fase 4b, cuando exista una conexión WebSocket real para
probarlo. Lo que Fase 1 sí puede confirmar de forma aislada es que la
request Mercury en sí funciona y devuelve algo parseable.

### 4.2 `ApResolve` — extensión chica, no reescritura

Hoy (`ApResolve.cpp`) solo resuelve `type=accesspoint`. Rust y Go piden
los tres tipos en una sola llamada:
`?type=accesspoint&type=dealer&type=spclient`, respuesta
`{"accesspoint": [...], "dealer": [...], "spclient": [...]}`. Extender
`ApResolve` para devolver los tres (o agregar un método
`fetchDealerAddress()`/`fetchSpclientAddress()` análogo al que ya
existe) es un cambio chico y localizado, no una reescritura.

### 4.3 `Context`/`MercurySession`/`TimeProvider` — sin cambios

`Context` ya tiene `deviceId`, `deviceName`, `username`, `session`
(`MercurySession`), `timeProvider` — todo lo que un `DealerClient`/
`PlayerEngine` nuevo necesitaría para identificarse, sin agregar
nada al contexto salvo, probablemente, el token de sesión de §4.1 una
vez obtenido.

### 4.4 `HTTPClient`/`SocketStream` — el PUT de connect-state los necesita, y ya sabemos que son portables

El PUT a `spclient` es HTTPS normal — `bell::HTTPClient` (ya evaluado a
fondo en `docs/host_tests.md`/F101 para el test de host) sirve tal
cual, sin cambios, para esta parte. Dato de continuidad entre este
trabajo y la suite de tests: si en algún momento se quiere testear el
PUT de connect-state en host, la misma infraestructura de
`fake_cdn_server.h` (servidor HTTP falso, Range/keep-alive) es
directamente adaptable a un "fake spclient" — no hay que inventar nada
nuevo ahí tampoco.

---

## 5. Qué es completamente nuevo

### 5.1 Esquema protobuf `connect-state`

`spirc.proto` (lo que ya tenemos) no sirve para nada de esto — el
esquema nuevo es `connectstate/connect.proto` (nombre confirmado en
ambos repos): `ClusterUpdate`, `PutStateRequest`, `PutStateReason`,
`Device`, `DeviceInfo`, `PlayerState`, `Capabilities`,
`TransferState`, `SetVolumeCommand`, etc. Este archivo `.proto` hay que
conseguirlo (está vendorizado en ambos repos investigados, se puede
tomar de ahí — son schemas públicos de Spotify, no algo propietario de
librespot) y generarlo con nanopb, igual que ya se hace con
`spirc.proto`/`mercury.proto`/etc. hoy.

### 5.2 Cliente Dealer (WebSocket)

Una clase nueva, `DealerClient`, responsable de: resolver el host
(§4.2), pedir el token (§4.1), conectar vía `WebSocketTransport` (§3.3
— `EspWebSocketTransport` en ESP32, RFC 6455 propio en host/test),
parsear el envelope `type: message|request|pong` (JSON — cJSON, ya
vendorizado, alcanza), decodificar `payloads[0]` (base64 —
`CryptoMbedTLS::base64Decode` — y gzip, que **no** tenemos hoy en
ningún lado del proyecto, ver §6.3), rutear por prefijo de URI o por
`message_ident`, mandar ping/responder pong, reconectar con backoff y
token fresco.

**Confirmado (2026-07-13): apoyarse en clases ya existentes para todo
lo que no sea el framing WebSocket en sí.** Esto es lo que
`DealerClient`/`WebSocketTransport` (la implementación RFC 6455
portable de §3.3, para host/test) reusarían tal cual, sin escribir
nada nuevo:

- **`bell::TCPSocket`/`TLSSocket`** (vía `bell::Socket`) — la conexión
  TCP+TLS de base. `wss://dealer.spotify.com` es, a este nivel, la
  misma conexión TLS que `CDNAudioFile`/`HTTPClient` ya abren hoy.
- **`bell::SocketStream`** — la capa `std::iostream` sobre el socket,
  para mandar el request de upgrade y después leer/escribir los frames
  crudos.
- **`picohttpparser.h`** (`phr_parse_response`) — para parsear la
  respuesta `101 Switching Protocols` + sus headers, llamado directo
  sobre el `SocketStream`, **sin pasar por `HTTPClient::Response`
  completo**: `HTTPClient` está pensado para el ciclo
  request→respuesta-con-body, y un `101` no tiene body — mezclar eso
  ahí adentro es más riesgo que llamar al mismo parser que
  `HTTPClient` ya usa por dentro, directamente.
- **`CryptoMbedTLS`** (`sha1Init`/`sha1Update`/`sha1FinalBytes`,
  `base64Encode`, `generateVectorWithRandomData`) — para
  `Sec-WebSocket-Key` (16 bytes random + base64) y validar
  `Sec-WebSocket-Accept` (SHA1+base64).
- **`bell::Task`** — la tarea de fondo que posee el loop de lectura del
  WS, mismo patrón que `MercurySession` ya es hoy.
- **`Queue<T>`** (`Queue.h`, ya confirmado portable en F101) — para
  entregar mensajes del Dealer ya parseados desde la tarea de red hacia
  quien los consume, mismo patrón que `MercurySession::packetQueue`.
- **`NanoPBHelper`/`pbDecode`** — para los payloads protobuf, una vez
  que exista el schema `connect-state` (§5.1), misma maquinaria que ya
  decodifica `spirc.pb.h` hoy.
- **`Utils.h`** (`hton64`, `pack<T>`/`extract<T>`) — para los campos de
  longitud del frame WS (variantes de 16/64 bits, big-endian).

Lo único genuinamente nuevo es el build/parse del header de frame WS en
sí (2-14 bytes: opcode, flag FIN, máscara, longitud) y la clase
`DealerClient` que orquesta todo lo de arriba — no una reimplementación
de ninguna de estas piezas.

### 5.3 `PlayerEngine` (o similar) — el reemplazo de `SpircHandler`

Ver §7 para la decisión de diseño concreta (clase nueva vs. reescribir
`SpircHandler` por dentro). Responsable de: mantener el `PlayerState`/
`DeviceInfo` local, reaccionar a los mensajes/requests que llegan del
`DealerClient` (cluster update, comando de player), y publicar el
estado vía PUT a `spclient` cada vez que algo cambia (con el mismo tipo
de rate-limiting/coalescing que Go hace, 200ms).

---

## 6. Desafíos técnicos identificados (no solo teóricos — cosas que van a doler si no se planean)

### 6.1 Reensamblado + backpressure del WebSocket, en FreeRTOS

Ya resuelto una vez, en `spotify_app` (§3.1) — hay que portar el
patrón (event group + buffer compartido), no reinventarlo. Riesgo real:
un mensaje de Dealer que llega mientras la tarea de reproducción está
ocupada (ej. un `readBytes()` bloqueante hacia el CDN) — el mismo tipo
de problema que F70/F76/F78 ya enseñaron en este proyecto: nunca
bloquear la tarea de red esperando a la tarea de reproducción, ni
viceversa, sin medir el impacto.

**Tamaño del buffer de reensamblado — dato real de `spotify_app`, no
estimado.** Ese proyecto usa `MAX_WS_BUFFER = 4096` (`spotify_client_priv.h:48`)
para el mismo tipo de mensaje (JSON rico en estado de reproducción,
aunque en su rol de controlador vía Web API, no de dispositivo). Tenía
un `assert()` que abortaba el firmware entero si un mensaje superaba
ese límite (`handler_callbacks.c:107`) — corregido el 2026-07-06 (su
`ANALYSIS.md`, hallazgo 1.5) para loguear y descartar en vez de
crashear. No hay registro de que el límite de 4KB se haya superado
realmente en tráfico real; el `assert()` era defensivo, no una
respuesta a un bug reportado.

Para el rol de `cspot` (dispositivo), el mensaje que más preocupa por
tamaño es `hm://connect-state/v1/cluster` (`ClusterUpdate`): puede
traer el `PlayerState` completo — metadata por track, *y* las colas
`next_tracks`/`prev_tracks` con metadata por ítem (decenas de entradas
en un contexto largo) — más la lista de todos los `Device` del
cluster. Esto puede irse a 10-20KB+ en el caso peor, muy por encima de
los 4KB de referencia. `player/command` (RPC `"request"`), en cambio,
suele ser chico (cientos de bytes a ~1KB), salvo un `"transfer"` que
arrastre contexto.

**Decisión de tamaño**: no copiar el número fijo de 4KB de
`spotify_app` — tiene sentido ahí por RAM interna acotada, no acá,
donde el buffer de reensamblado puede vivir en PSRAM. Sí copiar el
*patrón* ya corregido (loguear + descartar el mensaje que no entra,
nunca `assert`/abortar) — eso es lo reusable, no la constante. Tamaño
concreto del buffer: a definir en la Fase 4b con mensajes reales de
Dealer (mismo criterio empírico que gzip, §6.3).

### 6.2 Payload mixto: protobuf **o** JSON, según la URI

`payloads[0]` no tiene un único formato — hay que saber, por URI, si lo
que sigue después de base64+gzip es un protobuf `ClusterUpdate` o un
JSON plano (`social-connect/v2/session_update` en Rust). Esto necesita
una tabla de dispatch explícita (URI → parser), no un parser genérico.

### 6.3 gzip — CONFIRMADO: se resuelve empíricamente, en Fase 4b, no de antemano

`Transfer-Encoding: gzip` en los headers de un mensaje implica que el
payload, tras el base64-decode, hay que descomprimirlo antes de
parsearlo. **Este proyecto no tiene ninguna librería de
compresión/descompresión hoy** (ni zlib, ni miniz, nada). El soporte de gzip quedó deliberadamente afuera del
alcance inicial, la decisión de si hace falta se toma con datos
reales de la Fase 4b, y apuntando a esta sección del documento
(`docs/dealer_websocket_migration.md`, §6.3) para el contexto completo.

### 6.4 Correlación token-de-sesión ↔ conexión WS ↔ PUT

El `x-spotify-connection-id` que autoriza el PUT solo se obtiene
**después** de que el WebSocket entrega su primer mensaje — hay una
ventana real donde el dispositivo está "logueado" (AP) pero todavía no
"conectado" (Dealer) en la que no se puede publicar estado. Estado a
modelar explícitamente (no asumir que el PUT está siempre disponible).

### 6.5 Reconexión con token fresco

Igual que F58 (CDN) y F93/F101 (AP) ya enseñaron en este proyecto:
la reconexión del Dealer no puede reusar ciegamente el token/URL
inicial — Rust lo resuelve pasando la función de armado de URL como
closure, evaluada de nuevo en cada intento de reconexión, específicamente
para no pegarle a un token vencido. Mismo principio a aplicar acá:
nunca cachear la URL completa, recalcularla (token incluido) en cada
intento.

### 6.6 Rate-limiting/coalescing de los PUT salientes

Go hace como mucho un PUT cada 200ms, juntando ráfagas de cambios de
estado con un timer. Sin esto, una secuencia rápida de eventos locales
(ej. varios `SpircHandler::notify()`-equivalentes seguidos) generaría
un PUT por cada uno — no solo ineficiente, sino un patrón de request
que un backend real puede empezar a rate-limitear (429), que Go maneja
como un caso aparte con `Retry-After`.

**Implementado (2026-07-14)**: `PlayerEngine::runTask()` — el
mismo intervalo de 200ms que `go-librespot` (`statePutMinInterval`).
Reusa la infraestructura de coalescing que ya existía (el "último
valor gana" de `updatePlayerState()`/`hasPending`, agregado en la
Fase 5 para el problema de stack, §5.3) en vez de agregar un mecanismo
nuevo: si no pasaron 200ms desde el último PUT, espera lo que falta
y **vuelve a chequear** si llegó un estado más fresco durante esa
espera antes de mandar — así lo que sale a la red es siempre el
estado más reciente, no una foto vieja tomada al principio de la
espera. `putState()` (el PUT de registro, un solo tiro) queda **fuera**
de este límite a propósito — mismo criterio que `go-librespot`, cuyo
`NEW_DEVICE` tampoco pasa por `updateState()`/su coalescing. Build
limpio (50%), suite host 107/107 sin regresiones. Pendiente de
confirmar en hardware (debería verse, como mucho, un `PUT ok` cada
200ms incluso con eventos de SpircHandler llegando más seguido).

---

## 7. Decisión de diseño central — CONFIRMADO (2026-07-13)

**La pregunta era**: ¿la clase que reemplaza a `SpircHandler` es (a) el
mismo `SpircHandler` con las tripas reescritas (patrón que usa
librespot Rust: mismo nombre público, implementación 100% nueva
adentro), o (b) una clase completamente nueva (`PlayerEngine`)
que implementa una interfaz equivalente, y `cspot_connect.cpp` decide
cuál instanciar?

**Decisión: opción (b), clase nueva.** `PlayerEngine`, con sus
propias estructuras de datos (`PlayerState`/`DeviceInfo` del schema
`connect-state`, no `spirc.pb.h`'s `Frame`/`State`), sin heredar
ninguna decisión de diseño de la implementación SPIRC/Mercury vieja
solo por parecido superficial. Superficie pública equivalente a la de
`SpircHandler` hoy (`setPause`, `nextSong`, `previousSong`,
`getPositionMs`, `setRepeatContext`, `setRemoteVolume`,
`setEventHandler`, `getTrackPlayer`, `disconnect`), para que
`cspot_connect.cpp` (y la capa de UI/app detrás) no tenga que cambiar
en su forma de usarla.

---

## 8. Plan de implementación por fases

Cada fase termina en un punto donde conviene parar y confirmar antes de
seguir a la siguiente — ninguna fase asume que las siguientes ya están
aprobadas.

**Fase 1 — Token de sesión Mercury (§4.1)**
- Método nuevo, chico, sobre `MercurySession`/`AccessKeyFetcher` (o una
  clase hermana) que pida `hm://keymaster/token/authenticated?scope=...`
  y devuelva el token. Sin WebSocket todavía — esto es 100% reusar
  infraestructura Mercury existente. Verificable de forma aislada
  (logueando el token obtenido) antes de tocar nada de Dealer.

**Resultados (2026-07-13)**:
- Clase nueva `DealerTokenFetcher`
  (`components/cspot/cspot/include/DealerTokenFetcher.h`,
  `src/DealerTokenFetcher.cpp`), mismo patrón que `AccessKeyFetcher`
  (`isExpired()`/`getToken()`/`updateToken()`, parseo JSON dual
  cJSON/nlohmann detrás de `BELL_ONLY_CJSON`, igual que
  `AccessKeyFetcher.cpp`). Arma
  `hm://keymaster/token/authenticated?scope=streaming&client_id=<KEYMASTER_CLIENT_ID>&device_id=<ctx->config.deviceId>`
  y lo manda con `ctx->session->execute(MercurySession::RequestType::GET, ...)`.
  - `client_id` usa el ID público `65b708073fc0480ea92a077233ca87bd`
    (`KEYMASTER_CLIENT_ID` en `librespot`'s `core/src/config.rs`,
    confirmado leyendo el fuente real) — **no** `ctx->config.clientId`
    (el ID/secret propio de este proyecto, para el grant
    `client_credentials` no relacionado de `AccessKeyFetcher`).
  - `scope=streaming` es una primera aproximación, sujeta a revisión
    empírica en Fase 4b (ver §4.1) — no hay precedente real de qué
    scope exacto hace falta para este uso puntual.
- **Detalle de threading resuelto al implementar, no anticipado en el
  diseño original**: `MercurySession::execute()` es asíncrono — el
  callback de respuesta solo se dispara cuando *alguna* tarea llama a
  `handlePacket()` (que vacía `packetQueue`, llenada por la propia
  tarea `mercury_dispatcher` de `MercurySession`). En producción, quien
  llama a `handlePacket()` en loop es la tarea de sesión de
  `CSpotConnectPlayer::runSessionInner()`. Como `DealerTokenFetcher::updateToken()`
  bloquea en un semáforo hasta que ese callback le avise, llamarlo
  *desde esa misma tarea* haría deadlock (esperando una respuesta que
  nunca se procesa porque esa misma tarea está bloqueada esperándola en
  vez de seguir llamando a `handlePacket()`). Mismo principio que F93.
  Resuelto con una tarea `bell::Task` aparte
  (`DealerTokenProbeTask`, en `cspot_connect.cpp`) — mismo patrón que
  ya usa `TrackQueue` (tarea propia, `execute()` desde ahí, la tarea de
  sesión sigue drenando `handlePacket()` en paralelo).
- Cableado **temporal** en `cspot_connect.cpp`
  (`CSpotConnectPlayer::runSessionInner()`, justo después de
  `ctx->session->startTask()`): lanza `DealerTokenProbeTask`, que pide
  el token y lo loguea (largo + prefijo, no el token completo — es una
  credencial). Marcado explícitamente como código temporal de
  verificación de Fase 1 en comentarios en el propio código, con
  instrucción de removerlo una vez que `DealerClient`/Fase 4-5 posean
  este fetch de verdad.
- **Compila limpio**: `idf.py build` sin errores, margen de flash sin
  cambios relevantes (0x20ede0 B libres, 51% — prácticamente igual al
  51.5% ya medido en §9, la clase nueva es chica).
- **Primer resultado real en hardware (2026-07-13)**: la request
  Mercury en sí funciona (mismo mecanismo que el fetch de metadata de
  tracks, ya probado) — pero Spotify rechazó el primer intento con un
  error explícito y legible, no un fallo de transporte:
  ```
  E DealerTokenFetcher.cpp:101: Failed to parse Dealer keymaster token response: {"code":4,"errorDescription":"Invalid request"}
  ```
  Primer intento usaba `scope=streaming` (un solo scope). Corregido
  para usar el combo completo de scopes del único caller real
  encontrado en la investigación (`librespot`'s `examples/get_token.rs`:
  `streaming,user-read-playback-state,user-modify-playback-state,user-read-currently-playing`)
  — sin precedente de que un scope único alcance. Se agregó además un
  log de la URL exacta enviada (`DealerTokenFetcher.cpp`, antes del
  `execute()`) para tener más datos si este intento también falla.
- **Segunda vuelta en hardware (2026-07-13) — CONCLUSIÓN: keymaster
  descartado.** Mismo error exacto (`{"code":4,"errorDescription":"Invalid request"}`)
  con el scope completo de `examples/get_token.rs`. Con dos scopes
  distintos rechazados igual, el scope no era la causa — confirma la
  hipótesis que ya estaba en §4.1: el endpoint
  `hm://keymaster/token/authenticated` no es funcional para este
  `client_id`/tipo de sesión (ZeroConf + credencial guardada), sea
  porque el server lo rechaza para clientes no privilegiados, sea
  porque está efectivamente deprecado del lado servidor pese a seguir
  presente en el código de `librespot` como referencia histórica. **La
  sonda barata cumplió su propósito**: costó una clase chica y dos
  vueltas de hardware, no una implementación de Login5 entera, para
  descartar el camino más corto con certeza empírica en vez de
  asumirlo.
  **Siguiente paso: Login5** (ver la corrección de §4.1 más arriba
  para el alcance — `client_token` previo, schema `login5.proto`
  nuevo, posible resolución de desafíos hashcash).



**Fase 1b — Login5 (reemplazo del enfoque keymaster) — implementada
(2026-07-13), pendiente de hardware**

Investigación previa (leyendo los fuentes reales de nuevo, no de
memoria): el flujo completo que usan ambos clientes de referencia es
(1) `POST clienttoken.spotify.com/v1/clienttoken` (protobuf
`ClientTokenRequest` con client_id + device_id + datos de plataforma)
→ `Client-Token`; (2) `POST login5.spotify.com/v3/login` (protobuf
`LoginRequest` con `stored_credential`, header `Client-Token`) →
token de acceso, resolviendo desafíos hashcash si el servidor los
exige. Confirmaciones clave:

- **El camino está probado en producción para nuestro caso exacto**:
  go-librespot con ZeroConf (`ConnectBlob`, `session/session.go:176`)
  alimenta Login5 con `username` + `StoredCredentials()` del AP — lo
  mismo que cspot tiene en `ctx->config.username`/`authData`
  (credencial reusable del APWelcome). Y su `ClientIdHex`
  (`client_id.go`) es el mismo ID público `65b70807...` que keymaster
  rechazó — el ID es válido, era el endpoint el muerto.
- **El hashcash de go-librespot tiene un bug latente** (hasher SHA1
  acumulativo, nunca reseteado entre iteraciones — `login5/hashcash.go`);
  la semántica canónica es la de librespot Rust (`core/src/util.rs:129`,
  `solve_hash_cash`): SHA1 fresco por iteración sobre `prefix||suffix`,
  suffix de 16 bytes = [target+counter BE64, counter BE64] con
  target = BE64(sha1(login_context)[12..20]), éxito cuando los últimos
  8 bytes del digest (BE64) tienen >= `length` bits cero al final,
  timeout de 5s. **Se portó la de Rust.**
- **Upstream cspot ya traía un `login5.proto` mínimo** (sin usar, sin
  challenges) — se extendió en vez de crear otro: `HashcashChallenge/
  Solution`, `Challenge(s)`, `ChallengeSolution(s)`, `login_context`
  en request/response, `HashcashDuration` (stand-in wire-compatible de
  `google.protobuf.Duration`, para no importar well-known types en
  nanopb). `clienttoken.proto` es nuevo (package `clienttoken` para
  evitar colisiones con los schemas sin package), recortado a lo que
  se manda/recibe. Lección nanopb: con package declarado, las entradas
  de `.options` deben usar el nombre calificado
  (`clienttoken.Mensaje.campo`) o se ignoran en silencio (campos
  generados como `pb_callback_t` en vez de arrays estáticos — así se
  detectó).
- Implementación: `Login5Client.{h,cpp}` (mismo patrón que
  `AccessKeyFetcher`: `isExpired()/getToken()/updateToken()`, cache de
  ambos tokens con expiración, reintentos con `BELL_SLEEP_MS`, todo
  HTTPS envuelto en try/catch estilo F26/F63). **Sin dependencia de
  Mercury** — a diferencia de keymaster, no toca `handlePacket()`, así
  que no existe el riesgo de deadlock de la Fase 1 (igual corre en
  tarea propia para no frenar el arranque de sesión). Desafíos de
  clienttoken (distintos de los de login5): no soportados, igual que
  go-librespot en producción — se loguea claro si aparecen.
- **Segunda vuelta de hardware (2026-07-14)**: TLS ok, pero clienttoken
  devolvió `200` con body de **0 bytes** (4 reintentos iguales). Como
  el endpoint no requiere sesión, se reprodujo el request
  **byte-idéntico** al del device desde el host (socket TLS crudo) y se
  biseccionó la causa contra el endpoint real:
  - `Accept: application/x-protobuf` solo → `Content-Length: 322`, el
    token real adentro (protobuf `response_type=1` + `granted_token`).
  - `Accept: */*` **más** `Accept: application/x-protobuf` (lo que el
    device mandaba) → `Content-Length: 0`. El `Host:` con `:443` se
    probó aislado y es inocente.

  La causa: `bell::HTTPClient::rawRequest()` escribe **siempre** un
  `Accept: */*` hardcodeado y después agrega los headers del caller —
  cualquier caller que pase su propio `Accept` genera el duplicado, y
  el edge envoy de Spotify responde `200` vacío ante eso. Ningún
  caller previo pasaba `Accept` (por eso nunca se vio). **Fix en
  `HTTPClient.cpp`**: el default solo se escribe si el caller no trae
  un `Accept` propio (chequeo case-insensitive). Cubre también
  login5.spotify.com, que usa el mismo header. `idf.py build` limpio
  (51%) y suite host 107/107 tras el fix. Pendiente de tercera vuelta
  de hardware.
- **Tercera vuelta de hardware (2026-07-14): CONFIRMADO — Fase 1b
  cerrada con éxito.** Con el fix del `Accept` duplicado, el flujo
  completo funciona en la placa: clienttoken otorgado → login5
  autenticado con la credencial del AP → token de usuario obtenido.
  La autenticación del Dealer está resuelta; la sonda
  `Login5ProbeTask` se reemplaza por la de la Fase 4b, que es quien
  consume el token de verdad.

**Fase 2 — `ApResolve` extendido (§4.2)**
- Agregar `type=dealer`/`type=spclient` a la resolución existente.
  Cambio chico, testeable con el mismo patrón de host-test que ya
  existe (`docs/host_tests.md`) si se justifica.

**Resultados (2026-07-13)**: hecho — helper privado
`fetchFirstAddressOfType(type)` (`GET apresolve.spotify.com/?type=X`,
respuesta keyed por nombre de tipo, a diferencia del `ap_list` de la
forma legacy sin query, que queda intacta para no tocar el flujo AP
probado) + `fetchFirstDealerAddress()`/`fetchFirstSpclientAddress()`
públicos. Mismo hardening F36 (nunca indexar sin chequear), ambas
ramas cJSON/nlohmann. Compila limpio; se verifica contra el servicio
real en la Fase 4b, que es su primer consumidor.

**Fase 3 — Esquema protobuf `connect-state` (§5.1)**
- Conseguir/adaptar `connectstate/connect.proto` (y sus dependencias -
  revisar si importa otros `.proto` propios de librespot que haya que
  traer también), generarlo con nanopb, confirmar que compila junto al
  resto sin colisión de nombres con `spirc.proto`.

**Resultados (2026-07-13)**: hecho — `connectstate.proto` (package
`connectstate`, proto3 — a diferencia del resto proto2, porque
spclient espera semántica proto3: escalares en cero no se
serializan). Recorte deliberado del schema completo (527 líneas en
go-librespot) a lo que `PlayerEngine` realmente necesita:
encode (`PutStateRequest` → `Device`/`DeviceInfo`/`Capabilities`/
`PlayerState`/`ProvidedTrack`) con arrays estáticos (stack-friendly,
sin bookkeeping de malloc), decode (`ClusterUpdate`/`SetVolumeCommand`)
con `FT_POINTER` (un string más largo de lo previsto jamás rompe el
decode contra un max_size fijo — se libera con `pb_release`).
**Fuera del recorte, documentado para poder agregarse después sin
romper nada** (números de campo idénticos al schema completo, campos
omitidos se saltean solos): colas `prev_tracks`/`next_tracks`, maps de
metadata, `Restrictions`, `Cluster.player_state` y el map de devices
del cluster. Genera y compila limpio junto a `spirc.proto` sin
colisiones.



- **Verificación contra servidor independiente**: los servidores de
  eco públicos no son alcanzables desde este entorno (DNS restringido),
  así que se usó la variante local que el plan ya contemplaba —
  `tests/ws_echo_server.py` sobre la librería python `websockets`
  15.0.1 (implementación mainstream y estricta del RFC: exige masking
  del cliente, corre close handshake real, y con `ping_interval=1,
  ping_timeout=2` desconecta a cualquier cliente que no responda sus
  pings de protocolo). Test: `tests/ws_transport_echo_test.cpp`
  (target nuevo en el CMake de la suite host, ver
  `docs/host_tests.md`). Resultado: **ALL PASS** — handshake
  (Sec-WebSocket-Accept verificado), eco intacto con longitudes de
  7 bits (25 B), 16 bits (30 KB) y 64 bits (80 KB), 4s de idle
  sobreviviendo ciclos de ping del servidor (prueba real del pong),
  ping propio, disconnect limpio.
- **El spike encontró un bug real antes de que llegara a producción
  de tests**: `disconnect()` hacía `stream.close()` (destruye el
  socket) con el reader thread todavía bloqueado dentro de `read()`
  sobre ese objeto — use-after-free, segfault reproducible. Fix:
  `::shutdown()` del fd para desbloquear al reader, `join()`, y recién
  entonces `close()`; requirió exponer `getFd()` en
  `bell::SocketBuffer`/`SocketStream` (agregado chico al vendored
  bell, comentado en el header). Exactamente el tipo de esquina que
  §3.3 predecía y la razón de probar el framing aislado primero.
- Build ESP y suite host completa re-verificados tras el cambio de
  `SocketStream.h`: `idf.py build` limpio (51%), unit_tests 26/26
  (107 aserciones), f93 y f87 sin regresiones.


- **Conexión**: `ApResolve::fetchFirstDealerAddress()` (Fase 2) +
  token de `Login5Client` (Fase 1b, confirmado en hardware) →
  `wss://{host}/?access_token={token}` vía `WebSocketTransport`
  (Fase 4a). **La URL nunca se cachea** — cada intento re-resuelve y
  re-pide el token (§6.5), con backoff exponencial 5s→60s cortado en
  rebanadas de 250ms para que `stop()` no espere una ventana entera.
- **Recepción (alcance 4b, solo observación)**: parsea el envelope
  JSON (cJSON directo, sin rama nlohmann — código nuevo de este
  proyecto, que siempre compila `BELL_ONLY_CJSON`); extrae y guarda el
  `Spotify-Connection-Id` del push `hm://pusher/v1/connections/`
  (expuesto vía `getConnectionId()`, para el PUT de la Fase 5); loguea
  todo lo demás (`message` con su URI y tamaño, `request` con su
  `message_ident`) sin interpretarlo. **La observación de gzip de
  §6.3 está cableada**: cualquier mensaje con `Transfer-Encoding` en
  sus headers se loguea y descarta, con el comentario en el código
  apuntando a §6.3 como estaba mandado.
- **Lifetime**: mismo patrón que `MercurySession` (F93) — el
  destructor toma un mutex que `runTask()` retiene toda su vida, así
  `dealer.reset()` en el teardown de sesión nunca libera el objeto
  bajo una tarea viva.
- **Cableado**: en `cspot_connect.cpp`, `runSessionInner()` crea el
  `DealerClient` después de `session->startTask()` (convive con SPIRC
  como observador hasta que `PlayerEngine` lo reemplace, Fase
  5/6) y lo para antes del teardown. La sonda `Login5ProbeTask` de la
  Fase 1b se eliminó — el token ahora lo consume el Dealer de verdad.
- Build: limpio, binario +31KB (ahora `esp_websocket_client` +
  `DealerClient` se linkean de verdad — antes el linker los podaba por
  no tener llamadores), 51% de flash libre.
- **Qué mirar en el log de hardware**: `Dealer: connected to ...` →
  `Dealer: got Spotify-Connection-Id (...)` → conexión estable (sin
  reconexiones espurias) → y si aparece algún
  `Transfer-Encoding` en pushes reales (cierra §6.3). Un 401 en el
  connect indicaría problema de scope/token (revisar
  `DEALER_TOKEN_SCOPE`... no aplica: login5 no usa scopes; sería
  token vencido/inválido).

**CONFIRMADO en hardware (2026-07-14)**: el flujo completo funciona en
la placa:
```
Login5Client.cpp:175: clienttoken granted (expires after 1216800s)
Login5Client.cpp:231: login5 authenticated (token expires in 3600s)
DealerClient.cpp:80: Dealer: connected to guc3-dealer.spotify.com:443
DealerClient.cpp:174: Dealer: got Spotify-Connection-Id (200 chars)
```
Conexión estable, connection-id capturado. **La app se queda en
"Connecting to ESP32-S3 Spotify"** — comportamiento esperado al cierre
de 4b: el dispositivo está conectado al push pero **todavía no publicó
su estado**; sin el primer PUT `PutStateRequest(NEW_DEVICE)` a
spclient (con el `X-Spotify-Connection-Id`), la app nunca lo ve
"aparecer". Eso es la primera tarea de la Fase 5. gzip: no se observó
`Transfer-Encoding` todavía (esperable — sin registro no llegan
`cluster` updates; se re-observa una vez que el PUT registre el
device).

**Fase 5 — `PlayerEngine` (o la opción elegida en §7)**
- Publicar el primer PUT (`PutStateReason_NEW_DEVICE`).
- Manejar `hm://connect-state/v1/cluster` (pausar si otro dispositivo
  toma control — el análogo directo del caso `kMessageTypeNotify` que
  `SpircHandler::handleFrame()` ya maneja hoy).
- Manejar `hm://connect-state/v1/player/command` (play/pause/seek/next/
  prev/transfer) con su reply `{"success":...}`.
- Rate-limiting/coalescing de los PUT salientes (§6.6).

**Resultados Fase 5 — primer paso (registro del device), implementado
(2026-07-14), pendiente de hardware**

Estudiado el PUT real leyendo `go-librespot` (`spclient/spclient.go`
`PutConnectState`, `daemon/player_state.go` `initState`/`putConnectState`):
`PUT https://{spclient}/connect-state/v1/devices/{device_id}`, headers
`Authorization: Bearer {token-login5}`, `Client-Token: {clienttoken}`,
`X-Spotify-Connection-Id: {connId}`, `Content-Type: application/x-protobuf`,
body protobuf `PutStateRequest`, éxito = `200`.

Implementado:
- `HTTPClient`: helper `put()` (miembro + estático), espejo exacto de
  `post()` — `rawRequest()` ya aceptaba método arbitrario.
- `Login5Client`: expone `getClientToken()` (el PUT necesita el
  `Client-Token` además del Bearer de acceso — ambos ya se obtenían
  adentro, solo faltaba exponer el primero).
- `PlayerEngine` (`include/`+`src/`) — **la clase nueva de §7,
  primer incremento**: arma el `PutStateRequest` mínimo de registro
  (`member_type=CONNECT_STATE`, `put_state_reason=NEW_DEVICE`,
  `is_active=false`, `DeviceInfo` con `Capabilities`, `PlayerState`
  mínimo estilo `State.reset()` de go-librespot) y hace el PUT.
  `getPositionMs`/`setPause`/etc. de la superficie SPIRC llegan en los
  incrementos siguientes; este primer paso es solo publicar estado.
- **Decisión de diseño que cierra parte de §6.3**:
  `Capabilities.supports_gzip_pushes = false` a propósito. Anunciar que
  **no** soportamos gzip hace que el server mande los `cluster` updates
  **sin comprimir** — así `DealerClient` puede leerlos sin
  descompresor, y §6.3 (gzip diferido) queda no como una limitación
  latente sino como una decisión activa y correcta. Se pasa a `true`
  solo cuando/si se implementa gzip.
- Cableado: `DealerClient` crea el `PlayerEngine` (comparte el
  `Login5Client` como `shared_ptr`) y, al capturar el
  `Spotify-Connection-Id`, hace `setConnectionId()` + `putState(NEW_DEVICE)`.
  Eso es lo que debería sacar a la app del "Connecting...".
- Build limpio (50%), suite host 107/107 tras los cambios a
  `HTTPClient`.
- **Qué mirar en hardware**: tras `got Spotify-Connection-Id`, un
  `connect-state PUT ok (reason 3)`, y que el device **aparezca y
  quede seleccionable** en la app (deja de decir "Connecting..."). Un
  `PUT failed, status 4xx` mostraría el `error_type/message` del server
  para diagnosticar.

**CONFIRMADO en hardware (2026-07-14)**:
```
Login5Client.cpp:184: clienttoken granted (expires after 1216800s)
Login5Client.cpp:240: login5 authenticated (token expires in 3600s)
DealerClient.cpp:82: Dealer: connected to guc3-dealer.spotify.com:443
DealerClient.cpp:176: Dealer: got Spotify-Connection-Id (200 chars)
PlayerEngine.cpp:123: connect-state PUT ok (reason 3)
```
spclient aceptó el registro (200). El pipeline nuevo completo —
Login5 → Dealer WS → connection-id → PUT connect-state — está
verificado de punta a punta en la placa, conviviendo con SPIRC (que
sigue manejando la reproducción real: en el mismo log se ve un Load
frame + playback vía Mercury en paralelo, sin interferencia).
**Observación pendiente para el siguiente incremento**: en este log no
aparecieron pushes `hm://connect-state/v1/cluster` post-registro —
falta provocar un cambio de cluster (p. ej. pausar desde el teléfono)
y ver si llega el `Dealer: message uri=hm://connect-state/v1/cluster`,
que es el insumo para decidir cómo encarar el manejo de cluster y
cerrar del todo la observación de gzip (§6.3; con
`supports_gzip_pushes=false` deberían llegar sin comprimir).

**Resultados Fase 5 — segundo incremento (cluster + player/command),
implementado (2026-07-14), pendiente de hardware**

En vez de esperar a la observación manual antes de escribir nada, se
adelantó el manejo — la decodificación es necesaria de todos modos
para *ver* qué trae un cluster update real (incluido si viene con
`Transfer-Encoding`, el dato que falta para §6.3).

- **`PlayerEngine::handleClusterUpdate(payload)`** (nuevo):
  decodifica el protobuf `ClusterUpdate` (`connectstate.proto`, ya
  generado) y loguea `update_reason`/`active_device_id`. Todavía
  observación pura — reaccionar (pausar si otro dispositivo toma
  control, el análogo de `kMessageTypeNotify` de `SpircHandler`) queda
  para el próximo incremento, una vez que haya datos reales de forma
  de un cluster para diseñar contra algo concreto en vez de a ciegas.
- **`DealerClient::handleMessage()`** extendido:
  - `hm://connect-state/v1/cluster` → `Crypto::base64Decode(payloads[0])`
    → `PlayerEngine::handleClusterUpdate()`.
  - `type:"request"` (antes solo logueado) → decodifica
    `payload.compressed` (base64 → JSON `{command:{endpoint,...}}`),
    loguea el `endpoint`, y **siempre responde**
    `{"type":"reply","key":...,"payload":{"success":false}}` vía
    `sendReply()` — un `"request"` sin respuesta deja al cliente real
    esperando un timeout (§1.3). El mismo chequeo de
    `Transfer-Encoding` de §6.3 aplica acá también (antes solo cubría
    `"message"`), con el mismo criterio: no adivinar, loguear y no
    intentar decodificar.
  - **`success:false` es deliberado, no un placeholder olvidado**:
    `SpircHandler` sigue siendo quien maneja la reproducción real hoy
    (confirmado en el log — Load frame + playback vía Mercury al mismo
    tiempo que el Dealer). Ejecutar el comando acá además de que SPIRC
    ya lo procesa por su cuenta correría dos controladores sobre el
    mismo estado. Responder honesto ("no lo hice") hasta que Fase 6
    haga el reemplazo es la opción correcta, no accidental.
- Build limpio (50%), suite host 107/107 sin regresiones (no se tocó
  ningún archivo compartido con la suite en este incremento).
- **Qué mirar en hardware**: con el device ya registrado (`PUT ok`),
  pausar/reanudar desde el teléfono debería producir
  `Dealer: cluster update, reason=... active_device_id=...` — el
  primer dato real de forma de un cluster de esta cuenta, y con eso se
  puede diseñar la reacción real (reemplaza la incógnita anterior).

**Primera vuelta de hardware con registro ya confirmado
(2026-07-14)**: se probó pausando desde el teléfono
(`SpircHandler.cpp:351: External pause command` en el log — la pausa
llegó por el canal **viejo**, SPIRC/Mercury). **Cero líneas `Dealer:`**
en los ~16s siguientes a `got Spotify-Connection-Id` — ni
`cluster update`, ni siquiera `unhandled`/`unparseable` (que sí
habrían aparecido ante cualquier mensaje mal formado). Tampoco
`connection lost, reconnecting` — el WS seguía vivo (el ping/pong
automático de `esp_websocket_client`, 10s por default, lo habría
detectado si hubiese caído). Conclusión: no es un mensaje mal
recibido, es que **no llegó nada**.

**Hipótesis, no confirmada**: el dispositivo sigue siendo, para el
teléfono, un dispositivo "nativo SPIRC" (la suscripción Mercury a
`hm://remote/user/.../v23` sigue activa y es la que de verdad maneja
la sesión) — el registro connect-state es paralelo. Es plausible que
Spotify no emita un `ClusterUpdate` para un cambio de estado que
ocurre en el mismo dispositivo vía SPIRC; el cluster típicamente se
actualiza cuando cambia el dispositivo *activo*, no en cada
play/pausa de quien ya lo está. **Sin confirmar** — pendiente de un
test más fuerte: transferir la reproducción hacia/desde este
dispositivo usando el selector Connect del teléfono (eso sí debería
producir un diff de cluster real, `active_device_id` cambiando).

**Bug real encontrado al revisar el código, independiente de la
hipótesis de arriba**: `EspWebSocketTransport::handleData()` (§4a)
declaraba un mensaje completo apenas el payload del frame WS actual
terminaba de bufferearse (`payload_offset + data_len >= payload_len`),
**sin mirar `data->fin`**. Ese campo (documentado en
`esp_websocket_client.h`: "Fin flag") es lo que de verdad marca el
último frame de un mensaje fragmentado a nivel WS (frames de
continuación, no el chunking interno por `buffer_size` que sí estaba
cubierto). Si el Dealer alguna vez manda un mensaje grande fragmentado
en varios frames WS, el código viejo habría entregado cada frame como
si fuera un mensaje completo — un bug real, aunque no necesariamente
la causa del silencio de esta vuelta (no hubo ni un log de mensaje mal
formado, así que lo más probable es que simplemente no haya llegado
nada, no que haya llegado algo mal cortado). **Corregido**: ahora solo
entrega a la cola cuando `data->fin` es true, acumulando entre frames
si no lo es.

**Diagnóstico agregado para la próxima vuelta**: un heartbeat de
idle en `DealerClient::runTask()` — loguea
`Dealer: connected, idle Ns (no server pushes)` cada 30s sin
mensajes, para no depender de comparar timestamps a ojo la próxima
vez. Build limpio (50%) tras ambos cambios.

**Segunda vuelta de hardware (2026-07-14) — el heartbeat confirma el
silencio**: `Dealer: connected, idle 30s (no server pushes)` — 30s
completos de conexión viva sin un solo push, incluida otra pausa vía
SPIRC en el medio. Cero ambigüedad: no es un problema de timing de la
prueba anterior, es sistemático.

**Causa encontrada leyendo la fuente real de `go-librespot`, no
especulación**: `daemon/player_state.go` mantiene un flag `active`
interno (`State.setActive()`) que se pone en `true` recién cuando el
dispositivo *empieza a reproducir de verdad*, y ese flag viaja como
`PutStateRequest.IsActive` en **cada** PUT posterior —
`putConnectState(ctx, PutStateReason_PLAYER_STATE_CHANGED)` se llama
en cada cambio de estado del player (`daemon/player.go:813`), no solo
en el registro inicial. Nuestro `PlayerEngine` mandaba
`is_active=false` hardcodeado y **un solo PUT en toda la sesión** (el
de `NEW_DEVICE`). Desde la perspectiva de connect-state, somos un
miembro registrado pero permanentemente inactivo — sin una señal de
reproducción real por ese canal, no hay nada que Spotify tenga motivo
para empujarnos.

**Experimento implementado (2026-07-14), no la solución definitiva**:
`PlayerEngine::putState()` gana un parámetro `isActive`
(default `false`, no rompe el PUT de registro). En `DealerClient`,
justo después del `NEW_DEVICE`, se manda un segundo PUT de prueba —
`PutStateReason_PLAYER_STATE_CHANGED` con `isActive=true` — marcado
explícitamente en el código como experimento, **no** todavía
conectado al estado real de reproducción de `SpircHandler` (eso
requiere bridging de sus eventos, un paso más grande, ya lindando con
la Fase 6). El objetivo es aislar una sola variable: ¿alcanza con
anunciarse activo para que empiecen a llegar `ClusterUpdate`, aunque
el `PlayerState` que mandamos sea mínimo/desactualizado?

Build limpio (50%). **Qué mirar en la próxima vuelta**: si tras el
segundo PUT empiezan a aparecer `Dealer: cluster update, reason=...`
(sin necesidad de tocar nada desde el teléfono, o al primer
play/pausa), la hipótesis queda confirmada y el paso siguiente es
diseñar el bridging real SpircHandler→PUT (Fase 6). Si sigue en
silencio, la causa es otra y hay que seguir buscando (candidatos:
`Capabilities.is_observable`/`is_controllable` mal seteados, o que
efectivamente los `ClusterUpdate` solo lleguen a clientes que además
consultan/observan el cluster activamente, no a cualquier miembro
registrado).

**Tercera vuelta de hardware (2026-07-14) — hipótesis descartada**:
`PlayerEngine.cpp:125: connect-state PUT ok (reason 4)`
confirma que el segundo PUT (`PLAYER_STATE_CHANGED`, `is_active=true`)
salió bien. Aun así, **90s de silencio total** (tres heartbeats de
30s seguidos), incluida otra pausa por SPIRC en el medio. `is_active=true`
solo, con un `PlayerState` casi vacío (sin track real, sin
`is_playing`), **no alcanza** — descartado como causa única.

**Por qué el test de pausar/reanudar por SPIRC no es decisivo, y cuál
sí lo sería**: las dos vueltas anteriores probaron indirectamente,
controlando la reproducción vía el canal viejo (remoto SPIRC hacia
`SpircHandler`). Eso nunca toca el dato que un `ClusterUpdate`
existe para comunicar — `Cluster.active_device_id` — porque el
concepto de "dispositivo activo" ahí adentro es responsabilidad de
SPIRC, no de connect-state, en esta sesión mixta. **El test que sí
fuerza un cambio de `active_device_id` real es transferir la
reproducción con el selector Connect del teléfono** (el ícono de
altavoz/cast arriba a la derecha en la app) — no pausar/reanudar,
sino elegir explícitamente OTRO dispositivo como destino (o, si ya
está sonando en otro lado, elegir "ESP32-S3 Spotify" desde ahí). Ese
es el próximo paso empírico antes de seguir modificando código a
ciegas — no tiene sentido seguir ajustando variables (capabilities,
más campos del `PlayerState`, etc.) sin antes confirmar que el
mecanismo de push en sí responde a *algo*.

**Cuarta vuelta de hardware (2026-07-14) — transferencia real
provocada, resultado mixto pero valioso**: durante la prueba llegó
tráfico real y variado del Dealer — `hm://playlist/v2/playlist/...`
(varios, mientras se navegaba la app), `playback-settings/content-settings-update`,
y en el momento exacto de la transferencia,
`social-connect/v2/broadcast_status_update`. Todo parseado y
ruteado correctamente (`unhandled in 5.2`, como se esperaba para URIs
sin manejador todavía). **Esto confirma lo que más importaba
técnicamente**: el pipeline completo — WS, JSON, base64, routing por
URI — funciona de punta a punta con tráfico real de producción, no
solo con el hello. El riesgo de diseño de §4a/§4b/§5.2 está saldado.

**Pero `hm://connect-state/v1/cluster` nunca apareció**, ni en ese
momento exacto. El evento de "otro player tomó control" llegó por
`social-connect/v2/broadcast_status_update` + el mecanismo viejo de
SPIRC (`SpircHandler.cpp:175`, comportamiento preexistente de
`cspot`, no tocado por esta migración) — no por connect-state.

**Verificado también** (leyendo `go-librespot` de nuevo): no hace
ningún GET explícito del cluster al arrancar — escucha pasivamente,
igual que nosotros. Eso descarta la hipótesis de que falte "sembrar"
el estado con una lectura inicial.

**Evaluación honesta en este punto**: con `is_active=true` + un
`PlayerState` casi vacío ya descartado, y con confirmación de que
el pipeline funciona pero el evento de cluster específicamente no
llega ni cuando debería, quedan dos explicaciones plausibles, no
distinguibles sin más instrumentación:
1. Falta un `PlayerState` *real* (track/context/`is_playing` de
   verdad, no un placeholder) para que el backend nos trate como
   participante legítimo del cluster — hasta este punto seguimos
   mandando datos mínimos/estáticos.
2. Los diffs de cluster podrían depender de que **otro** participante
   de la cuenta también sea connect-state-nativo — si el dispositivo
   que tomó control (el que generó el `social-connect` broadcast) es
   en sí mismo legacy/SPIRC del lado de Spotify, puede que el backend
   nunca calcule un diff de cluster para nadie en esa interacción,
   sin importar qué hagamos de nuestro lado.

La (2) no es corregible desde acá. La (1) sí, pero implica bridging
real de eventos de `SpircHandler` hacia `PlayerEngine` — una
pieza de arquitectura más grande, no otro experimento aislado y
barato como los anteriores.

**Decisión (2026-07-14, confirmada con el usuario)**: implementar el
bridging real (opción 1), en vez de pausar la investigación. Es
trabajo que hace falta de todos modos para la Fase 6 (el reemplazo de
`SpircHandler` necesita, tarde o temprano, que algo alimente
`PlayerState` con datos reales) — no es esfuerzo tirado aunque no
destrabe los cluster updates.

**Implementado (2026-07-14), pendiente de hardware**:

- **`TrackReference::encodeURI()`** (nuevo, `TrackReference.h`/`.cpp`) —
  inverso de `decodeURI()`, que ya existía ahí mismo (conversión de
  base grande, dividiendo repetidamente por 62 en vez de multiplicar).
  `bigNumDivide()`/`bigNumMultiply()` (`Utils.h`) no exponen el resto
  que hace falta acá, así que se lleva la cuenta directo en vez de
  reusarlos. `TrackInfo.trackId` (`TrackQueue.cpp`) ya es
  `bytesToHexString(gid)` — hex, no el URI base62 real — así que hacía
  falta ida y vuelta (`stringHexToBytes` → `encodeURI`), no estaba
  disponible en ningún lado del proyecto todavía.
- **`PlayerEngine`**: refactorizado — la maquinaria de armar y
  mandar el PUT (chequeo de connection-id/tokens, `DeviceInfo`,
  encode, HTTP, manejo de status) se separó a un privado
  `sendPutStateRequest()`, compartido por `putState()` (registro,
  sin cambios de comportamiento) y el nuevo
  **`updatePlayerState(isPlaying, trackUri, positionMs, durationMs)`**
  — arma un `PlayerState` real (`is_playing`/`is_paused`,
  `playback_speed`, `timestamp`, posición, duración, `track.uri` si
  no está vacío) y siempre manda `is_active=true` — a diferencia de
  `putState()`, que sigue con `false` por default para el registro.
  `DeviceInfo` ahora viaja en **cada** PUT, no solo el de registro —
  coincide con cómo lo hace `go-librespot`.
- **Cableado en `cspot_connect.cpp`**: `eventHandler()` (que ya recibe
  los eventos reales de `SpircHandler`, protegido por `spircMutex` —
  F62) gana dos nuevos efectos secundarios:
  - `TRACK_INFO`: reconstruye la URI real desde `info.trackId`, la
    cachea (`currentTrackUri`/`currentTrackDurationMs`, miembros
    nuevos de `CSpotConnectPlayer`, sin lock propio porque
    `eventHandler()` ya corre serializado bajo `spircMutex`
    independientemente de qué tarea lo invoque) y llama a
    `dealer->updatePlayerState(...)`.
  - `PLAY_PAUSE`: llama a `dealer->updatePlayerState(...)` con el
    nuevo estado de pausa y los valores cacheados de track/duración.
  - Ambos casos toman la posición real de `spirc->getPositionMs()`
    (ya extrapolada por SPIRC), no un valor inventado.
- Build limpio (50%), suite host 107/107 sin regresiones (ninguno de
  los archivos tocados es parte de `CSPOT_SHARED_SOURCES`).
- **Qué mirar en la próxima vuelta de hardware**: los logs
  `connect-state PUT ok (reason 4)` deberían empezar a aparecer en
  cada track/play/pausa (no solo una vez), y — la pregunta real — si
  eso alcanza para que aparezcan `Dealer: cluster update, reason=...`.
  Si tampoco aparecen con `PlayerState` real, la explicación (2) del
  punto anterior (el otro participante de la cuenta tampoco es
  connect-state-nativo) queda como la más probable, y en ese caso no
  hay más que ajustar de este lado — el trabajo de bridging igual
  queda hecho y es la base real para la Fase 6.

**Quinta vuelta de hardware (2026-07-14) — crash real, causa
encontrada y corregida**: los PUTs con `PlayerState` real sí
funcionaron (`connect-state PUT ok (reason 4)` repetido en cada
track/play/pausa, confirmando que el bridging llegaba). Pero al tocar
el botón físico de play/pausa en la pantalla, **stack overflow** en
la tarea `player_control_task`.

**Causa**: esa tarea (`components/ui/player_screen.cpp`) tiene 4KB de
stack, dimensionados para llamar a `spirc->setPause()` — barato, sin
red. `eventHandler()` (que ya corre en *cualquier* tarea que dispare
un evento de `SpircHandler`, por diseño — F62) ahora también llamaba,
en el mismo caso `PLAY_PAUSE`, a `dealer->updatePlayerState()`, que
hacía el PUT **síncrono** (HTTPS+TLS) ahí mismo. Con un botón remoto
(vía SPIRC/Mercury) eso corre en la tarea de sesión (32KB, sin
problema); con el botón físico, corre en `player_control_task` (4KB)
y desborda — el mismo tipo de error que ya había pasado con
`Login5ProbeTask` (8KB insuficientes para TLS), pero esta vez en una
tarea que no es nuestra y que no tiene sentido agrandar solo para
absorber una llamada de red que no debería estar ahí.

**Fix real, no un parche de tamaño de stack**: `PlayerEngine`
pasa a ser su propia `bell::Task` (32KB, mismo patrón que
`DealerClient`/`Login5Client`/`TrackQueue` para todo lo que toca
HTTPS). `updatePlayerState()` deja de ser síncrona — ahora solo
guarda el último estado pendiente (mutex + condition_variable,
"gana el más reciente", mismo criterio de coalescing que ya usa este
proyecto en otros lados) y notifica; el PUT de verdad lo hace
`runTask()`, en su propia tarea, nunca en la del que llama. Con esto,
`updatePlayerState()` es seguro de llamar desde cualquier tarea —
incluida una de 4KB — sin volver a arrastrar este problema a otro
caller futuro. `sendPutStateRequest()` (ya factorizado en el
incremento anterior) quedó intacto, solo cambió quién lo invoca y
desde dónde.

Lifecycle: `PlayerEngine` gana `stop()`/destructor con el
mismo patrón F93 que `DealerClient` (mutex que `runTask()` retiene
toda su vida); `DealerClient::stop()` ahora también llama a
`connectState->stop()`.

Build limpio (50%), suite host 107/107 sin regresiones. Pendiente de
sexta vuelta de hardware — con esto ya no debería haber overflow al
tocar los controles físicos, y sigue pendiente la observación de si
el `PlayerState` real (que sí llega) alcanza para destrabar los
cluster updates.

**Sexta vuelta de hardware (2026-07-14) — sin crash, y aparece algo
nuevo e importante**: los controles físicos ya no crashean. Los PUTs
con `PlayerState` real siguen confirmándose (`PUT ok (reason 4)`
repetido). Y por primera vez aparecieron requests reales:
```
DealerClient.cpp:281: Dealer: request ident=hm://connect-state/v1/player/command endpoint=? (execution deferred to Fase 6, replying success:false)
```
**Esto es significativo**: `hm://connect-state/v1/player/command` es
el canal real de comandos remotos del protocolo moderno — el
reemplazo directo de los `kMessageType*` de SPIRC (§1.4). Que
llegue de verdad es la primera señal de que la cuenta **sí** interactúa
con connect-state para algo — actualiza (sin todavía resolver del
todo) la hipótesis de la vuelta anterior sobre por qué los cluster
updates seguían en silencio.

**Pero la decodificación del comando falló en silencio**: `endpoint=?`
— no se pudo extraer `command.endpoint` del payload. Sin
`Transfer-Encoding` (así que no es gzip, un dato en sí mismo), la
causa exacta queda sin determinar entre varias posibles (el campo
`payload.compressed` no está donde se esperaba, el base64 decodifica
distinto, el JSON no tiene la forma `{command:{endpoint,...}}`
asumida de `librespot-go`'s `RawMessage`). **Se agregaron diagnósticos
en cada punto donde esto puede fallar en silencio** (`payload.compressed`
ausente, base64 a 0 bytes, JSON inválido, o `command.endpoint` ausente
del JSON ya parseado) — cada uno logueando el contenido crudo
recibido, para dejar de adivinar y ver la forma real en la próxima
captura. Build limpio (50%).

**Nota aparte, sin resolver**: los timestamps de los tres `request`
en este log (00:00:38, 00:00:56, 00:01:08, 00:01:15, 00:01:23) no son
periódicos parejos — podría ser el usuario tocando algo repetidas
veces, o el Dealer reintentando el mismo comando porque seguimos
respondiendo `success:false`. No hay suficiente dato todavía para
distinguir una cosa de la otra.

**Séptima vuelta de hardware (2026-07-14) — el fix async aguanta bajo
uso real**: ~6 toggles reales de play/pause vía SPIRC
(`External pause/play command`) durante la sesión, cada uno con su
`connect-state PUT ok (reason 4)` — **cero crashes**, confirmando que
mover el PUT a la tarea propia de `PlayerEngine` resolvió el
problema de raíz, no solo el síntoma puntual del botón físico. El
reinicio al final del log es un reflasheo normal (`rst:0x1 POWERON`,
`boot:0x20 DOWNLOAD(USB/UART0)`, `waiting for download` — la
secuencia de `esptool` esperando el nuevo binario), no un crash. No
llegó ningún `player/command` en esta ventana, así que los
diagnósticos del punto anterior siguen sin ejercitarse — pendiente de
una vuelta más que incluya uno.

**Octava vuelta de hardware (2026-07-14) — CONFIRMADO: forma real del
payload de `"request"`, distinta de la asumida**. Llegaron dos
`player/command` reales (17s de diferencia, mismo `endpoint`, mismo
`sent_by_device_id` — consistente con reintento por nuestro
`success:false`, no con dos acciones distintas del usuario):
```json
{"message_ident":"hm://connect-state/v1/player/command",
 "payload":{"message_id":1639675773,"target_alias_id":null,
   "sent_by_device_id":"299d17d41bb08980e81913fc37a146c92cb7dc55",
   "command":{"endpoint":"resume","options":{...},"logging_params":{...}}},
 "type":"request","key":"7c87130f-d14b-459c-834e-d88b7c08d911"}
```
**`payload` no es `{"compressed":"<base64>"}`** (la forma que asumí,
sacada de la struct Go de `librespot-go`) — en el caso sin comprimir,
`payload` **es directamente** el objeto del comando, ya decodificado.
La forma `compressed` debe existir solo cuando el servidor realmente
comprime (gzip), algo que todavía no se observó.

**Corregido en `DealerClient::handleMessage()`**: intenta
`payload.compressed` primero (por si alguna vez aparece, comprimido
de verdad); si no está, usa `payload` mismo como el objeto del
comando — sin decode adicional. Diagnósticos existentes conservados
para los casos que sigan fallando. Build limpio (50%).

`sent_by_device_id` no coincide con nuestro propio `device_id`
(`142137fd...`) — es otro dispositivo real de la cuenta (el teléfono,
u otro cliente) pidiéndonos `resume` vía connect-state — confirmación
adicional de que el pipeline de comandos remotos del protocolo
moderno funciona de punta a punta, con datos reales, no solo en
teoría. La ejecución real de `resume` (y los demás endpoints) sigue
en Fase 6, según lo ya decidido.

**Fase 6 — Integración con `cspot_connect.cpp`**
- Decisión de §7 aplicada: instanciar la implementación nueva (en
  paralelo a la vieja, o reemplazándola, según lo que se confirme en
  Fase 0).
- Verificación en hardware real, siguiendo el mismo flujo de este
  proyecto (`idf.py build` limpio → confirmar en hardware → iterar).

**Decisión de alcance (2026-07-14, confirmada con el usuario) — MVP
antes que corte completo**: el comando `Transfer` (cargar una
cola/contexto nuevo vía connect-state) necesita un schema protobuf
entero que todavía no está vendorizado (`TransferState`/`Queue`/
`Context`, `connectstate/transfer.proto` — 77 líneas, con lista
completa de tracks). Implementarlo de una junto con el resto era la
opción de mayor riesgo: reemplaza el único mecanismo de carga de cola
que hoy funciona en hardware real, a ciegas. Se optó por un MVP más
chico y seguro en su lugar — ver "Resultados" abajo. `Transfer` (y
`play`, `set_queue`, `add_to_queue`, que también necesitan
Queue/Context) quedan explícitamente para un incremento posterior.

**Aclaración sobre "sin coexistencia" (§7, §9 punto 3)**: esa decisión
era sobre no mantener **dos motores de reproducción independientes**
calculando cada uno su propio estado — no sobre prohibir que dos
*canales de entrada* (SPIRC y connect-state) alimenten el mismo motor.
El MVP de abajo hace exactamente eso: un solo `SpircHandler` (con su
`TrackQueue`/`TrackPlayer`/`PlaybackState`, intactos y sin tocar) sigue
siendo el único motor real; `PlayerEngine` no tiene motor
propio, solo reenvía comandos connect-state hacia los mismos métodos
`spircMutex`-protegidos que ya usan los botones locales de la UI. No
hay dos estados que puedan divergir — hay un estado, con una entrada
más.

**Resultados — MVP de ejecución de comandos, implementado
(2026-07-14), pendiente de hardware**:

- **`SpircHandler::seekMs(uint32_t)`** (nuevo método público, chico y
  aditivo): la lógica de `handleFrame()`'s `kMessageTypeSeek` se
  extrajo tal cual a un método público, sin cambiar su comportamiento
  — mismo patrón que ya usan `setPause`/`nextSong`/`previousSong`
  (wrappers públicos sobre el motor interno).
- **`PlayerEngine` gana `CommandCallback`**
  (`std::function<bool(endpoint, cJSON* command)>`) +
  `setCommandCallback()` + `handlePlayerCommand()` — un pass-through
  deliberado, sin lógica propia todavía (queda como el punto de
  extensión natural para cuando `Transfer` sí se implemente acá).
- **`DealerClient`**: el `"request"` de `player/command` ahora llama a
  `connectState->handlePlayerCommand(endpoint, command)` de verdad
  (antes: `sendReply(key, false)` fijo) y responde con el resultado
  real. `command` (el cJSON del comando, incluidos campos como
  `position`/`value`) se pasa mientras sigue vivo, antes de liberar la
  copia parseada del payload base64 (si vino por esa rama).
- **`cspot_connect.cpp`**: `handlePlayerCommand(endpoint, command)`
  traduce los endpoints del MVP a los wrappers `spircMutex`-protegidos
  ya existentes (`requestPlayPause`, `requestNext`, `requestPrevious`,
  el nuevo `requestSeek`, `requestSetRepeatContext`):
  - `pause` → `requestPlayPause(false)`
  - `resume` → `requestPlayPause(true)` (el endpoint que ya vimos
    llegar en vivo, Fase 5 "octava vuelta")
  - `skip_next`/`skip_prev` → `requestNext()`/`requestPrevious()`
  - `seek_to` → `requestSeek(position)` — **solo posición absoluta**;
    si `command.relative` viene no vacío, se rechaza explícitamente
    (loggeado) en vez de calcular mal una posición relativa sin haber
    verificado la semántica exacta contra tráfico real.
  - `set_repeating_context` → `requestSetRepeatContext(command.value)`
  - Todo lo demás (`transfer`, `play`, `update_context`,
    `set_repeating_track` — cspot no tiene repeat por track, ver el
    comentario ya existente en `cspot_connect.h` sobre esta limitación
    deliberada —, `set_shuffling_context` — cspot no tiene shuffle —,
    `set_options`, `set_queue`, `add_to_queue`) responde `false` con
    un log claro de "no implementado, diferido", nunca a ciegas.
- **Sin riesgo de repetir el bug de stack de §5.3**: la ejecución
  corre en la tarea propia de `DealerClient` (32KB, ya apta para red)
  y las llamadas `request*()` son locales/baratas (sin red adentro de
  `spircMutex`) — no hay ninguna llamada HTTPS en esta cadena.
- Build limpio (50%), suite host 107/107 sin regresiones.
- **Qué mirar en hardware**: el `resume`/`pause` que ya sabemos que
  llega debería ahora `-> success=1` en el log (antes siempre
  `success=false`) y **controlar la reproducción de verdad**. Probar
  también `seek_to`/`skip_next`/`skip_prev`/repeat desde un cliente
  connect-state-nativo si es posible — más allá de confirmar que
  compila, esto necesita prueba real de audio, no solo mirar logs.

**CONFIRMADO en hardware (2026-07-14)**: funciona. El MVP de
ejecución de comandos (Fase 6) queda cerrado — comandos remotos vía
connect-state controlan la reproducción real, a través del mismo
`SpircHandler` que ya la controlaba por SPIRC. Con esto, el
dispositivo tiene las dos mitades del protocolo moderno funcionando
de punta a punta: Login5 → Dealer WS → registro connect-state →
PlayerState real publicado → comandos remotos ejecutados. Lo que
queda (`Transfer`/carga de cola nueva, reacción a cluster updates,
rate-limiting de PUTs §6.6) son incrementos, no bloqueantes para un
dispositivo Connect funcional.

**Hallazgo de eficiencia post-confirmación (2026-07-14), corregido**:
el usuario preguntó directamente si esta versión hace más requests
HTTP — la respuesta llevó a encontrar una ineficiencia real, no solo
"sí, un poco más". `PlayerEngine::sendPutStateRequest()`
(usado por cada PUT — cada track, cada play/pausa, cada comando
remoto, no solo una vez por sesión como Login5) hacía:
1. Un `apresolve.spotify.com?type=spclient` **nuevo en cada llamada**,
   pese a que la dirección de spclient no cambia de un PUT al
   siguiente.
2. Una conexión TCP+TLS **nueva** cada vez (`bell::HTTPClient::put()`
   crea un `Response` local que se destruye — y cierra el socket — al
   salir de la función), a diferencia de `CDNAudioFile`, que sí reusa
   conexión (`reused=1` en sus logs).

Dos TLS handshakes completos por actualización de estado, donde
debería haber cero en el caso normal.

**Corregido**: `spclientHost` se cachea (se resuelve una vez, se
reusa hasta que una request falle de verdad — no ante un simple
no-200 como un 429); `putConnection` mantiene un
`bell::HTTPClient::Response` vivo entre llamadas, mismo patrón que
`CDNAudioFile` (la lógica de reconexión-si-hace-falta de
`rawRequest()`, ya endurecida por F58/F82, cubre sola el caso de que
el servidor cierre la conexión keep-alive en el medio).

**Bug real encontrado al reusar la conexión, corregido en
`HTTPClient.cpp` (no solo en `PlayerEngine`)**:
`readRawBody()` solo lee si `rawBody` está vacío
(`if (contentSize > 0 && rawBody.size() == 0)`) — en un `Response`
reusado para una SEGUNDA request, un body no vacío de una respuesta
anterior (p. ej. un mensaje de error) quedaba pisando `rawBody`, y la
siguiente respuesta real nunca se leía — `.body()`/`.bytes()`
devolvían el contenido viejo en silencio. `contentSize` tenía el
mismo problema al revés (una request sin body después de una CON
body se quedaba con el tamaño viejo, intentando leer bytes que el
servidor nunca mandó). Ninguna de las dos cosas se había visto antes
porque `CDNAudioFile` — el único otro reusador de conexión — nunca
llama a `.body()`/`.bytes()`, lee el stream directo. **Fix**:
`readResponseHeaders()` ahora resetea `rawBody`/`contentSize` al
principio de cada request, mismo lugar donde ya se resetean
`responseHeaders`/`httpBufferAvailable`. Beneficia a cualquier futuro
reusador de conexión que sí llame a `.body()`, no solo a este.

**Concurrencia real detectada al escribir el fix, corregida**: el PUT
de registro (`putState`, desde la tarea de `DealerClient`) y el PUT
de estado real (`updatePlayerState`, desde la tarea propia de
`PlayerEngine`) pueden llamar a `sendPutStateRequest()` desde
dos tareas distintas — sin lock, podían pisarse tanto los campos
cacheados como, más grave, la conexión compartida a mitad de una
request. `putMutex` serializa `sendPutStateRequest()` completo, no
solo el cacheo.

Build limpio (50%), suite host 107/107 sin regresiones tras los tres
cambios (`PlayerEngine.{h,cpp}`, `HTTPClient.cpp`). Pendiente
de confirmar en hardware que el ahorro es real (debería verse mucho
menos tráfico a `apresolve.spotify.com` en capturas de red, aunque
esto no es observable desde los logs seriales actuales).

**Bug real, autoinfligido, encontrado en la primera vuelta de
hardware con el fix de reuso puesto (2026-07-14)**:
```
PlayerEngine.cpp:164: connect-state PUT request failed: Cannot parse http response
```
justo después de un `PUT ok (reason 3)` — mismo mensaje que F82 (CDN),
pero esta vez provocado por el propio cambio de reuso de conexión: el
camino de éxito (`status == 200`) nunca llamaba a `.body()` — el body
de esa respuesta (aunque fuera chico o vacío-con-headers) quedaba sin
drenar en el socket, y la siguiente request en la misma conexión
keep-alive arrancaba leyendo la cola de la respuesta anterior en vez
del status line real. Confirma que `rawRequest()`'s reconnect-on-failure
(F58/F82) solo reintenta si el socket YA está marcado como no-`good()`
— acá el socket seguía viéndose sano (el problema era de framing, no
de conexión), así que no reintentaba solo y la excepción subía tal
cual. El PUT siguiente (`reason 4`) sí salió bien, confirmando que el
`catch` ya limpiaba `putConnection`/`spclientHost` correctamente — el
autorecupero funcionó, pero no debería haber hecho falta.

**Fix**: `sendPutStateRequest()` ahora drena el body **siempre**,
haya éxito o no, antes de devolver — no solo en la rama de error como
antes. `readRawBody()` lee exactamente `contentSize` bytes (ni más ni
menos), así que esto deja el socket bien posicionado para la próxima
request sin importar el tamaño del body. Build limpio (50%), suite
host 107/107 sin regresiones.

**Limitación conocida, no nueva de este cambio**: `HTTPClient` no
sabe leer `Transfer-Encoding: chunked` — si spclient alguna vez
respondiera así (sin `Content-Length`), el body seguiría sin
drenarse igual. No hay evidencia de que esto pase (las respuestas de
Spotify observadas hasta ahora usan `Content-Length`), y es una
limitación preexistente de toda la clase `HTTPClient`, no algo nuevo
de esta conexión reusada — queda anotado, no corregido, por no ser
parte de lo que esta vuelta encontró en la práctica.

**CONFIRMADO en hardware (2026-07-14)**: sin más "Cannot parse http
response" tras el fix de drenaje. Reuso de conexión + caching de
spclient funcionando correctamente en PUTs consecutivos.

**No incluido en el alcance de este plan** (a menos que se pida
explícitamente más adelante): `Transfer`/`play`/`set_queue`/
`add_to_queue` (necesitan el schema `TransferState`, diferido —
ver arriba), `spotify:user:attributes:*`,
`social-connect/v2/session_update`, manejo de playlists vía
`hm://playlist/v2/playlist/`, logout remoto (ni siquiera librespot lo
tiene implementado hoy, ver §1.4) — todos mensajes reales del
protocolo, pero no bloqueantes para tener un dispositivo Connect
funcional con control remoto básico.

---

## 9. Estado de las decisiones (actualizado 2026-07-13) y lo que sigue sin medir

Las cuatro preguntas que este documento tenía abiertas ya se cerraron:

1. ~~§6.3 (gzip)~~ — **resuelto**: no se decide de antemano, `DealerClient`
   soporta solo el caso no-gzip desde el día uno, y la decisión de si
   hace falta más se toma con datos reales en la Fase 4b (ver §6.3 y §8
   para el detalle, incluida la nota de dejar el comentario en el
   código en el punto donde se lee `Transfer-Encoding`).
2. ~~§7 (clase nueva vs. reescritura de `SpircHandler`)~~ — **resuelto**:
   clase nueva, `PlayerEngine` (ver §7).
3. ~~Coexistencia temporal con SPIRC~~ — **resuelto**: sin etapa de
   correr las dos en paralelo — reemplazo directo una vez que
   `PlayerEngine` esté probada en hardware (ver §7).

**Flash, ya medido y resuelto** (no es una decisión de diseño, es un
dato): `esp-tls`/`esp_http_client` ya están linkeados hoy por el fetch
de carátulas de `components/ui`, así que no son costo nuevo. Margen
actual medido: 2.11MB libres (~51.5%) — amplio para el schema protobuf
nuevo + la lógica de Dealer/ConnectState.

Lo único que **sigue sin poder confirmarse de antemano**, porque
depende de código real corriendo en la placa, no de una decisión de
diseño:

4. **RAM**: una sesión TLS más (la del Dealer) corriendo en paralelo a
   la del CDN, del orden de 16-40KB de buffers de sesión de mbedTLS. La
   placa tiene PSRAM y el proyecto ya maneja varias sesiones
   concurrentes, así que es manejable en principio — pero se confirma
   en hardware real una vez que haya algo compilando, mismo flujo de
   siempre en este proyecto (compilar limpio → confirmar en hardware),
   no antes.

---

## 10. Fase 6 "corte completo" — retiro de SPIRC (2026-07-14)

Con la Fase 6 MVP ya confirmada en hardware (Dealer + `PlayerEngine`
recibiendo/ejecutando comandos reales, `player/command` respondiendo con
`{"success":...}` reales), se decidió ir por el retiro completo de SPIRC en
vez de dejarlo en su mínima expresión — motivo explícito: ahorrar recursos
en el SoC (flash/RAM/red), no solo simplificar código. Se optó por la
opción más grande: reescribir `TrackQueue` también (sacando su dependencia
de `PlaybackState`) en vez de solo borrar `SpircHandler.cpp` y seguir
reusando `TrackQueue`/`TrackPlayer`/`PlaybackState` tal cual.

**Expectativa de ahorro, dicha explícitamente para no generar falsas
expectativas**: modesto, no dramático. El binario pasó de ~52% a ~50%
libre durante toda esta migración (schema protobuf nuevo, DealerClient,
PlayerEngine, ContextResolver se sumaron ANTES de sacar nada de
SPIRC). El ahorro real de este corte no viene tanto de borrar
`spirc.pb.h` (nanopb genera código chico para un schema chico) sino de
eliminar el tráfico Mercury pub/sub duplicado (`hm://remote/user/.../v23`)
y el estado que dejaba de calcularse dos veces.

**Investigación previa** (antes de tocar código): mecanismo real de
`Transfer`/context-resolve en go-librespot, clonado a un scratchpad y
leído directamente (no asumido) — confirmado que un `Transfer` command
NO trae la lista de tracks inline (a diferencia del Load frame de SPIRC):
el cliente debe pedir `GET /context-resolve/v1/{uri}` a spclient y seguir
`page.next_page_url`/`page.page_url` para paginar
(`spclient/context_resolver.go`, `spclient/spclient.go:392`). La respuesta
de ese endpoint es **JSON**, no protobuf wire format — pero el payload de
un `Transfer` command en sí (`command.data`, base64 dentro del JSON del
Dealer) sí es protobuf wire format real de `TransferState`
(`proto/spotify/connectstate/transfer.proto`), confirmado leyendo
`daemon/player.go`'s `case "transfer":` directamente. Dos formatos
distintos en el mismo flujo — importante no asumir que todo lo que toca
`Context`/`ContextTrack` es JSON solo porque el context-resolve HTTP lo es.

**Análisis de acoplamiento `TrackQueue`↔`PlaybackState`** (lectura completa
de ambos archivos antes de tocar nada): solo 4 puntos de contacto reales,
de los cuales solo 2 eran dependencias funcionales genuinas (el
load-input de `updateTracks()` y la posición leída en `skipTrack()` para
decidir "PREV reinicia el track actual si <3s"). Los otros 2 eran
bookkeeping de salida redundante (el callback de encode para frames SPIRC
salientes, y dos escrituras de `playing_track_index` que nadie más leía).
Este análisis redujo bastante el riesgo/alcance real del refactor frente
a lo que parecía de entrada.

**Cambios**:
- `TrackQueue`: constructor/`skipTrack()`/`updateTracks()` ahora reciben
  la lista de tracks, el índice inicial y la posición actual como
  parámetros explícitos, en vez de leerlos implícitamente de un
  `PlaybackState` compartido. Toda la lógica interna (precarga, consumo,
  skip, repeat-context) intacta.
- `PlaybackEvent.h` (nuevo): `EventType`/`EventData`/`Event`/`EventHandler`
  extraídos de `SpircHandler` a un header compartido — `SpircHandler`
  mantiene los mismos nombres anidados vía `using`, sin tocar ningún call
  site existente.
- `ContextResolver` (nuevo): cliente de `context-resolve/v1/{uri}` +
  paginación + parseo cJSON→`TrackReference` (mapeo canónico JSON de
  protobuf, sin schema nanopb — igual que `DealerClient`'s propio parseo).
- `PlayerEngine` ahora posee su propio `TrackQueue`/`TrackPlayer` y
  hace su propio tracking de posición (extrapola mientras reproduce,
  congela al pausar — misma matemática que `PlaybackState` pero sin el
  frame SPIRC alrededor). Ya no reenvía comandos a `SpircHandler` vía
  `CommandCallback` (eliminado) — ejecuta `pause`/`resume`/`skip_next`/
  `skip_prev`/`seek_to`/`set_repeating_context`/`transfer` directamente
  contra su propio motor.
- `TransferState`/`Playback`/`Session`/`Context`/`ContextTrack` (nuevos,
  `connectstate.proto`): schema nanopb mínimo a propósito — solo los
  campos que `PlayerEngine` realmente lee (uri de contexto,
  posición/pausa, track actual para encontrar el índice de arranque en la
  lista resuelta). Todo lo demás (queue, play_origin, suppressions...) se
  saltea solo en el decode nanopb, sin romper nada.
- Volumen remoto: `SetVolumeCommand` (ya existía en el schema, sin usar)
  ahora se decodifica en `DealerClient::handleMessage()` (mensaje
  `hm://connect-state/v1/connect/volume`) y se aplica en
  `PlayerEngine::handleSetVolume()`.
- Reacción a cluster ("otro dispositivo tomó control"): antes vivía en
  `SpircHandler`'s manejo del frame Notify de Mercury; ahora es
  `PlayerEngine::handleClusterUpdate()` comparando
  `cluster.active_device_id` contra `ctx->config.deviceId` — se perdía
  funcionalidad real si no se reimplementaba, no era opcional.
- `cspot_connect.cpp`: reescrito para usar
  `dealer->getConnectState()` como motor principal — ya no crea/usa
  `SpircHandler` en absoluto. `spircMutex` eliminado (ya no hace falta:
  `TrackQueue`/`TrackPlayer` ya protegen su propio estado internamente,
  a diferencia del `SpircHandler` original que no tenía locking propio).

**Estado**: compila limpio (build completo, 50% flash libre), pero
`SpircHandler`/`PlaybackState`/`spirc.proto` **todavía no se borraron** —
siguen compilados pero sin usarse en runtime. Se borran solo después de
confirmar en hardware que el motor nuevo funciona igual de bien que el
viejo (login, reproducción, cola, skip/prev/seek, pausa/resume remoto y
local, Transfer real desde la app, volumen remoto, y la reacción a
"otro dispositivo tomó control"), siguiendo el mismo principio que el
resto de este documento: no confirmar de antemano lo que depende de
hardware real.

**Primera vuelta de hardware (2026-07-14)**: login, Dealer, PUT de
registro, y el primer `Transfer` real de la app funcionaron de punta a
punta - cola cargada (`updateTracks(initial)`), metadata/audio key/CDN
fetch, `TrackPlayer` reproduciendo de verdad ("Smooth Criminal"), PUT de
`PLAYER_STATE_CHANGED` confirmado. Pero un **segundo** `Transfer` (~12s
después, mismo dispositivo) falló: `context-resolve/v1/` con URI vacía →
404. La causa: `current_session.context.uri` puede venir como puntero real
a string vacío (no nulo) cuando la reproducción no está atada a un
contexto - **cola ad-hoc** (resultados de búsqueda, radio, una cola
armada a mano), donde la lista de tracks va directo en
`TransferState.queue.tracks` (ya resuelta, sin HTTP). El MVP original
saltaba `queue` a propósito ("deliberately minimal") asumiendo que todo
`Transfer` real traía contexto - la práctica mostró que no. **Fix**:
`Queue`/`TransferState.queue` agregados al schema nanopb;
`handleTransfer()` ahora usa `context.uri` solo si es no-vacío, y cae a
`queue.tracks` (convertidos a `TrackReference` con `pbArrayToVector()`
para `gid`, mismo patrón que `ContextResolver.cpp`'s `appendTracks()`)
en caso contrario. Build limpio tras el fix, 50% flash libre sin cambios.
Sigue pendiente confirmar en hardware: skip/prev/seek, pausa/resume local
y remoto, volumen remoto, reacción a "otro dispositivo tomó control", y
que este segundo Transfer (cola ad-hoc) ahora sí cargue y reproduzca.

**Segunda vuelta de hardware (2026-07-14)**: dos intentos de conexión
consecutivos fallaron - **todos** los `Transfer` (4 en total, en ambos
intentos) cayeron en "no context uri and no queue tracks", incluso el
primero justo después del PUT de registro. Causa real: el chequeo de
"¿hay TransferState inline?" solo miraba si el campo JSON `"data"`
**existía** (puntero no nulo), no si decodificaba a bytes reales -
`"data":""` (presente, pero string vacío) pasaba ese chequeo, y
`pb_decode()` sobre un buffer de 0 bytes "tiene éxito" trivialmente
dejando todo en su default, cayendo en la rama de error en vez de la
inofensiva. go-librespot chequea explícitamente `len(Command.Data) == 0`
(longitud ya decodificada), no la presencia del campo JSON - exactamente
lo que faltaba acá. **Fix**: el chequeo ahora decodifica primero y mira
`raw.empty()`, mismo criterio que go-librespot, para las dos formas de
"sin TransferState" (campo ausente o presente-pero-vacío). Se agregó
además un log de diagnóstico (`transfer: decoded N bytes, has_session=...
has_context=... context_uri='...' has_queue=... queue_tracks=...`) por si
esto vuelve a pasar por otra causa - así la próxima vez hay datos
concretos en vez de tener que volver a adivinar. Build limpio, 50% flash
libre sin cambios. **No confirmado todavía si `"data":""` era real el
motivo** - pendiente de una tercera vuelta de hardware con el log nuevo
para confirmarlo definitivamente antes de dar esto por cerrado.

**Tercera vuelta de hardware (2026-07-14)**: `"data":""` NO era el
motivo real - el log de diagnóstico mostró un `TransferState` real de 83
bytes, bien formado (`has_session=1 has_context=1 has_queue=1`), pero con
`context_uri=''` y `queue_tracks=0`. En vez de seguir adivinando, se
volcó el payload crudo en hex y se decodificó a mano, byte por byte
(wire format de protobuf) para tener la verdad definitiva en vez de otra
hipótesis: `options` (shuffle/repeat en false), `playback` (is_paused=true,
position_as_of_timestamp=7751, sin `current_track`), `current_session`
(`play_origin` con sus 7 campos string vacíos, `context` con
`uri=""`/`url=""`/`restrictions={}`, sin `pages`), y `queue` (sin el
campo `tracks` en absoluto, `is_playing_queue=false`, más un campo 3 no
documentado en el `transfer.proto` de go-librespot - dos varints, probablemente
específico de la app real de Spotify y no del fork open-source). Ningún
campo en todo el mensaje trae track alguno - es un `TransferState`
genuinamente vacío de contenido reproducible.

**Conclusión**: esto pasa cuando se selecciona el dispositivo desde el
picker de Connect sin que haya nada sonando en ningún lado - no hay nada
que transferir, pero Spotify igual manda el sobre completo (con
`is_paused`/`position`/`playback_speed` reales) en vez de un `data`
vacío. Tratar esto como error (`success=false`) hacía que el cliente
real de Spotify **desistiera** en vez de simplemente dejar el
dispositivo activo e inactivo. **Fix**: el caso "sin contexto y sin
queue" ahora es el mismo resultado que `raw.empty()` - éxito, sin cargar
nada, dispositivo activo esperando el próximo comando real. Build
limpio, 50% flash libre sin cambios. Pendiente confirmar en hardware que
esto realmente resuelve el "el cliente intenta y desiste", y que un
`Transfer` posterior con datos reales (context o queue con tracks) carga
y reproduce correctamente sobre este mismo device ya "activo".

**Cuarta vuelta de hardware (2026-07-14)**: con `success=1` confirmado
en el log, el dispositivo **igual** dejaba de estar seleccionado en la
app ("ni siquiera queda seleccionado") - el mismo síntoma que antes,
sin cambios pese al fix del reply. Esto descarta que el valor de
`success` fuera la causa real. Causa real encontrada: `is_active=true`
solo llega a spclient dentro de `runTask()`'s PUT, y eso solo se
despierta vía `updatePlayerState()` - que a su vez solo se llama desde
eventos reales de reproducción (`PLAYBACK_START`/`PLAY_PAUSE`/
`TRACK_INFO`, disparados por `TrackQueue`/`TrackPlayer`). Con un
Transfer vacío no hay track que cargar, así que ningún evento dispara
jamás, y el dispositivo se queda con `is_active=false` en spclient para
siempre - la app lee eso (no el ack del comando) para decidir si el
dispositivo sigue activo, así que lo desconecta sin importar qué
contestemos. **Fix**: los dos caminos de "transfer vacío, sin nada que
cargar" (`raw.empty()` y "sin context/queue") ahora llaman
`updatePlayerState(false, "", 0, 0)` explícitamente antes de retornar,
para que el PUT de `is_active=true` salga igual aunque no haya ningún
track. Build limpio, 50% flash libre sin cambios. Pendiente confirmar en
hardware que esto sí resuelve la desconexión.

**Quinta vuelta de hardware (2026-07-14)**: con el fix de `is_active`
funcionando, el dispositivo quedó seleccionado el tiempo suficiente para
que la app mandara un comando real `play` (no `transfer`) al intentar
reproducir algo. A diferencia de `transfer`, `play` lleva su contexto
como JSON plano directo en el objeto del comando
(`command.context.uri`, mismo mapeo canónico protobuf→JSON que ya usa
`ContextResolver` para las respuestas de `context-resolve` - no
protobuf en base64 como el `data` de `transfer`), confirmado contra
`daemon/player.go`'s `case "play":` de go-librespot. **Implementado**:
`PlayerEngine::handlePlay()` - extrae `context.uri`, resuelve
vía `contextResolver.resolve()` (mismo camino que `transfer`), busca el
índice de arranque en `options.skip_to.track_uri` (o `track_index` como
fallback; `track_uid` no soportado - `TrackReference` no tiene ese
campo), aplica `options.initially_paused`, y carga con `loadTracks()`.

**Sexta vuelta de hardware (2026-07-14), red estable - bug distinto**:
un `transfer` real con contexto (`spotify:track:...`) cargó "Black
Sheep" de punta a punta hasta `TrackPlayer.cpp:245: Playing`, pero
"nada se escucha, en el cliente no hay barra, estado ni track" - ni
audio ni metadata llegaban a la app, no solo silencio. Causa
encontrada leyendo `cspot_connect.cpp`'s `pcmWrite()`: el
`if (paused) return 0;` (heredado sin cambios del `pcmWrite()`
original, ya presente antes de esta migración) corre **antes** del
bloque que llama a `notifyAudioReachedPlayback()` (lo único que reporta
track info a la app) - así que un track que carga directamente en
pausa nunca llega a ese bloque, la app nunca se entera de que existe
un track. Con SPIRC esto casi nunca se disparaba en la práctica (el
Load frame casi siempre arrancaba en Play); con `Transfer`'s
`Playback.is_paused` sí, y bastante seguido - confirmado decodificando
a mano el payload real de 799 bytes: `is_paused=true` en el campo 4
del submensaje `playback`. **Fix**: se invirtió el orden en
`pcmWrite()` - el bloque de `trackChanged`/`notifyAudioReachedPlayback()`
corre siempre primero (sigue disparando una sola vez por track real
gracias al guard existente de F79), y **después** el
`if (paused) return 0;` sigue suprimiendo el audio real mientras siga
en pausa - separando "reportar que hay un track" de "efectivamente
sonar". Build limpio, 50% flash libre sin cambios. Pendiente confirmar
en hardware que la app ahora muestra track/barra/estado en pausa, y
que presionar play en la app efectivamente empieza a sonar.

**Nota**: el guard `isAlreadyLoaded()` (evitar reiniciar a mitad de
canción ante un segundo `transfer` duplicado para el mismo track, ver
la vuelta anterior sobre "Around the World") fue descartado por el
usuario junto con esa prueba (red inestable, no se pudo confirmar si
era un bug real o un artefacto de la red) - no está en el código
actual. Si la reproducción se corta/reinicia de nuevo bajo red
estable, es candidato a reimplementar.

**Octava vuelta de hardware (2026-07-14) - PRIMERA REPRODUCCIÓN
COMPLETA CONFIRMADA**: dispositivo seleccionado (empty transfer +
`is_active`), usuario eligió una playlist real en la app, comando
`play` real llegó y tuvo éxito, "Tu Misterioso Alguien" cargó, bajó de
CDN, y sonó - `TrackPlayer.cpp:245: Playing` + PUT `reason 4` con
track/posición real 200ms después. Cola siguió precargando (`La
Sunamita`, `Cold Little Heart...`) y el `CDN fetch` siguió reportando
`reused=1` de forma sostenida por más de 20s sin cortes - primera
prueba end-to-end genuinamente estable de la sesión.

**Observación, no bug de esta migración**: el usuario notó ~7s entre
el `play` exitoso y el audio real arrancando (metadata Mercury → audio
key → URL de CDN → handshake TLS de CDN, todo secuencial), suficiente
para que el cliente de Spotify mostrara un estado de "problemas
reproduciendo" antes de arrancar. Confirmado que esta misma latencia
(~7-9s) ya estaba presente en la primerísima prueba exitosa de hoy
("Smooth Criminal", antes de cualquier cambio de Fase 6) - no es una
regresión de este corte, es una característica preexistente del
pipeline de fetch de cspot. Decisión del usuario: no optimizar ahora,
seguir confirmando el resto del gate de hardware (skip/prev/seek,
pausa/resume local y remoto, volumen remoto, reacción a "otro
dispositivo tomó control") antes de tocar la latencia de arranque.

**Novena vuelta de hardware (2026-07-14) - seek_to rechazado**: arrastrar
la barra de progreso en la app mandó `seek_to` con
`relative='beginning', position=<n>` - rechazado
("only absolute position supported"). El cliente además mostró "Spotify
can't play this right now. If you have the file on your computer you
can import it." (posible efecto directo del comando fallido, a
confirmar). Causa: `relative` es un nombre engañoso - solo `"current"`
es un offset genuinamente relativo a la posición actual; `"beginning"`
y `""` (ausente) son AMBOS posición absoluta, solo que leen de campos
JSON distintos (`position` vs `value`) - confirmado contra
`daemon/player.go`'s `case "seek_to":` real de go-librespot. El código
anterior solo aceptaba `relative` vacío, rechazando de plano cualquier
`seek_to` real disparado arrastrando la barra en la app (que manda
`relative='beginning'`). **Fix**: reescrito para replicar exactamente
la lógica de go-librespot - `"current"` suma a `getPositionMs()`,
`"beginning"` usa `position` directo, `""` usa `value`. Build limpio,
50% flash libre sin cambios. Pendiente confirmar en hardware que
arrastrar la barra ahora funciona y que el mensaje de error del
cliente desaparece.

**Décima vuelta de hardware (2026-07-14) - reaparece el double-delivery,
`isAlreadyLoaded()` reaplicado**: escuchando en el cliente de
escritorio, el usuario seleccionó el ESP32-S3 e intentó transferir,
luego canceló - pero sonó en ambos dispositivos. En el log: un segundo
`transfer` (932 bytes) llegó para el mismo track que YA estaba sonando
("Around the World"), y como el guard `isAlreadyLoaded()` había sido
descartado (ver la nota más arriba), `loadTracks()` reseteó
incondicionalmente `TrackQueue`/`TrackPlayer` y reinició el track desde
0 - el ESP32 siguió sonando después de que el usuario pensó haber
cancelado la transferencia. **Reaplicado** `isAlreadyLoaded()` (mismo
diseño que antes: `lastLoadedTrackUri` seteado en `loadTracks()`,
chequeado en `handleTransfer()`/`handlePlay()` antes de recargar) -
esta vez con evidencia de que es un problema real y recurrente, no un
artefacto de red inestable. Build limpio, 50% flash libre sin cambios.

**Nota abierta**: este log no incluye ningún `cluster update` cerca del
momento del cancelado - no se pudo confirmar si `handleClusterUpdate()`
(la reacción a "otro dispositivo tomó control", §7 de este documento)
disparó o no cuando el desktop volvió a ser el dispositivo activo. El
fix de `isAlreadyLoaded()` evita el reinicio audible, pero no explica
por sí solo si el ESP32 debería haberse pausado al cancelar - pendiente
de un log completo (desde la selección hasta el cancelado) para
confirmar esa parte por separado.

**Duodécima vuelta de hardware (2026-07-14) - causa raíz encontrada
comparando contra las DOS implementaciones de referencia**: los nuevos
logs descartaron las dos hipótesis anteriores - keys distintas
(`473a51d9...` vs `60cfcd94...`, solicitudes independientes, no
reintento del transporte) y ningún "failed sending reply" (las
respuestas salieron bien). Con el transporte descartado, se revisó el
flujo completo conexión→reproducción contra go-librespot Y
librespot-rust (clonado a scratchpad para esto): **cada PUT de ambas
implementaciones lleva `PutStateRequest.last_command_message_id` +
`last_command_sent_by_device_id`**, grabados de CADA comando manejado
antes de ejecutarlo (go: `p.state.lastCommand = &req` en
`handlePlayerCommand()`, leído en `putConnectState()`; rust:
`set_last_command(request.clone())` en
`handle_connect_state_request()`, state.rs:289). Ese es el mecanismo
con el que el cliente que MANDÓ el comando correlaciona el estado de
cluster que recibe de vuelta con su propio comando - la respuesta
`{"success":true}` del Dealer NO alcanza; el cliente espera ver su
`message_id`/`device_id` ecoados en el próximo estado publicado.
Nosotros nunca poblábamos esos campos (estaban en el schema desde el
principio, campos 7/8, sin uso) - así que el desktop nunca vio su
transfer reconocido y se quedó en "conectando..." para siempre,
reintentando con transfers nuevos. **Fix**: (1)
`PlayerEngine::setLastCommand()` nuevo, llamado desde
`DealerClient` para todo request antes de ejecutarlo (extrae
`message_id`/`sent_by_device_id` del envelope JSON); (2)
`sendPutStateRequest()` los ecoa en cada PUT; (3) de paso,
`started_playing_at` (campo 9) también se manda ahora mientras el
device está activo (epoch ms de cuándo se volvió activo, solo
estampado en la transición false→true), igual que `activeSince`/
`active_since` en ambas implementaciones. No portado (anotado, no
olvidado): `has_been_playing_for_ms` (campo 11) y
`PlayerState.session_id` - librespot-rust tiene un comentario
explícito (spirc.rs ~1007) de que probar `session_id` no arregló su
propio problema de desync, y ninguno de los dos parece ser parte del
mecanismo de ack. Build limpio, 50% flash libre sin cambios.
Pendiente confirmar en hardware que la app ahora sale de
"conectando..." y deja elegir música.

**CONFIRMADO en hardware (2026-07-14)**: con el eco de
`last_command_message_id`/`last_command_sent_by_device_id` la app sale
de "conectando...", el flujo selección→play→audio real funciona de
punta a punta de forma repetible. Queda una aspereza conocida, solo en
la PRIMERA reproducción de la sesión: la demora acumulada de la primera
vez (apertura de WebSocket, tokens, context-resolve, metadata, audio
key, CDN + TLS - todo secuencial) hace que Spotify muestre "Spotify
can't play this right now..." antes de que el audio arranque igual;
las reproducciones siguientes ya no lo muestran (conexiones/token ya
calientes). Es la misma latencia de arranque preexistente ya anotada
(vuelta octava) - decisión vigente: no optimizar todavía, es cosmética
y no bloquea el gate.

---

## 11. Cierre del corte: SPIRC eliminado (2026-07-14)

Con el gate de hardware completo (usuario confirmó: seek_to,
pausa/resume remoto, skip next/prev, volumen remoto, "otro dispositivo
tomó control", y encadenado de tracks - todo funcionando), se ejecutó
el borrado final más un último repaso contra las dos implementaciones
de referencia:

**Repaso final contra go-librespot y librespot-rust** (una sola
diferencia funcional encontrada): al volverse inactivo por takeover,
ambas implementaciones avisan al server con un PUT al endpoint
dedicado `/connect-state/v1/devices/{id}/inactive` (go:
`PutConnectStateInactive()` desde `stopPlayback()`, espera 204; rust:
`became_inactive()`) - nosotros solo parábamos el audio localmente,
dejando estado stale en el cluster. **Agregado**:
`PlayerEngine::putStateInactive()` (reusa el mismo
putMutex/host cacheado/conexión keep-alive de los PUTs normales, body
vacío, `notify=false` como go-librespot), llamado desde el camino de
takeover de `handleClusterUpdate()`.

**Eficiencia**: `ContextResolver` ahora cachea el host de spclient
entre llamadas (antes pagaba un apresolve.spotify.com por CADA fetch -
uno por transfer/play Y uno por página de contexto paginado), con la
misma política de "limpiar solo ante fallo de transporte" que los PUTs.
Recorta parte de la latencia de primera reproducción (la del "Spotify
can't play this right now...").

**Borrado**: `SpircHandler.{h,cpp}`, `PlaybackState.{h,cpp}`,
`spirc.proto`, `spirc.options` eliminados. `TrackReference` perdió sus
codecs SPIRC (`pbEncodeTrackList`/`pbDecodeTrackList`, que serializaban
el mensaje `TrackRef` de spirc.proto) y los miembros `context`/`queued`
que solo esos codecs usaban - quedó sin dependencia protobuf alguna
(solo gid/uri/type + base62). Con esto muere también la suscripción
Mercury `hm://remote/user/.../` (vivía en `SpircHandler::
subscribeToMercury()`). `MercurySession` se queda: metadata, audio
keys, country code y time sync siguen pasando por ahí (ver §"qué sigue
usando Mercury" arriba). `tests/CMakeLists.txt` dejó de compilar
`spirc.pb.c`.

**Nota honesta sobre flash**: el binario NO se achicó con el borrado
(quedó igual, ~50% libre) - el linker ya venía descartando el código
SPIRC muerto vía `--gc-sections` desde que `cspot_connect.cpp` dejó de
referenciarlo (ese ahorro ya estaba bancado en la vuelta del recableo).
El borrado es higiene de código fuente + no compilar/mantener ~900
líneas muertas, no flash adicional. El ahorro de RED (no más pub/sub
`hm://remote` ni doble cálculo de estado) sí venía del recableo y sigue.

**Verificación**: build de firmware limpio (fullclean + build, 50%
libre) y suite de host completa en verde (unit_tests 26 casos/107
asserts, f87 y f93 con TSan limpios) tras regenerar el build dir de
tests. Pendiente de una última pasada de humo en hardware (conectar,
reproducir, pausar) para cerrar la migración del todo.

**Post-cierre, mismo día - mensaje Dealer de 54KB descartado**: en la
pasada de humo apareció el comportamiento errático que faltaba
explicar: `EspWebSocketTransport.cpp: WebSocket message too big (54624
bytes), dropping` - la placa venía descartando mensajes enteros del
Dealer que superaban el tope de reensamblado de 32KB (puesto en Fase 4b
con la nota explícita de "revisit with real data" - ese momento llegó).
Un cluster update de 54KB descartado deja a la placa ciega al estado
que decidió el server: siguió reproduciendo su cola vieja, y la app,
al no ver nunca su estado reflejado, desistió y reprodujo local.
go-librespot pone su read limit en ilimitado
(`SetReadLimit(math.MaxUint32)`, dealer.go:110) - en un micro un tope
duro sigue teniendo sentido, pero dimensionado para que el tráfico
real no lo toque. **Fix doble**: (1) tope subido a 256KB (~5x sobre el
mensaje más grande observado; con `CONFIG_SPIRAM_USE_MALLOC` +
`ALWAYSINTERNAL=512` el buffer de reensamblado cae en PSRAM, no en RAM
interna); (2) de paso se cerró un agujero real del chequeo original:
solo validaba el `payload_len` del PRIMER frame WS - un mensaje lógico
fragmentado (múltiples frames, fin=0) crecía sin límite alguno; ahora
el chequeo es acumulativo sobre el total reensamblado. Build limpio,
50% libre. Pendiente repetir la pasada de humo.

**CONFIRMADO en hardware (2026-07-15)**: con el tope de 256KB la pasada
de humo pasó limpia - sin mensajes descartados, sin comportamiento
errático. **La migración SPIRC→Dealer/connect-state queda CERRADA**: el
protocolo moderno completo (Login5 → Dealer WS → connect-state →
motor propio de PlayerEngine) es el único camino, confirmado de
punta a punta en hardware real, y SPIRC ya no existe en el árbol.

Deuda conocida, diferida a propósito (por orden de visibilidad):
1. Latencia de primera reproducción ~7s (cadena secuencial
   context-resolve → metadata Mercury → audio key → CDN+TLS) - el
   "Spotify can't play this right now" transitorio de la primera vez.
2. `set_queue`/`add_to_queue`/`update_context` responden
   `success=false` (la app se resincroniza sola, pero editar la cola
   desde la app no se refleja en la placa).
3. gzip pushes (`supports_gzip_pushes=false`) - los pushes llegan sin
   comprimir, funciona, pero mensajes grandes gastan más ancho de banda.
4. Shuffle (`set_shuffling_context`) - no-op desde siempre en cspot,
   ahora explícitamente no implementado.

---

## 12. Post-cierre: latencia de primer play + edición de cola (2026-07-15)

Los items 1 y 2 de la deuda diferida, pedidos explícitamente por el
usuario tras confirmar la migración estable.

**Latencia de primera reproducción** (dos recortes en la parte
spclient de la cadena; la parte CDN - el handshake TLS de ~3s con
audio-ak - no se puede precalentar porque el host CDN solo se conoce
al resolver el primer track):
- `ContextResolver` ya no hace su propio apresolve en el primer
  resolve: `PlayerEngine` le siembra el host
  (`seedSpclientHost()`) al resolverlo para el PUT de registro, que
  siempre ocurre antes de que pueda llegar ningún transfer/play. El
  host ahora va protegido por `hostMutex` (el seed llega desde la
  tarea de PlayerEngine; los fetch corren en la de
  DealerClient).
- `ContextResolver` reusa una conexión keep-alive entre fetches (una
  por sesión en vez de un handshake TLS por fetch - beneficia páginas
  de contextos paginados y todos los resolves posteriores al primero),
  con el mismo patrón drain-always/reset-on-transport-failure de los
  PUTs (la lección del "Cannot parse http response" ya aprendida ahí).

**`set_queue`/`add_to_queue` implementados** (antes respondían
`success=false`): dos operaciones nuevas en `TrackQueue` que editan la
lista ALREDEDOR del track sonando sin tocarlo (sin reload, sin
reinicio audible - a diferencia de `updateTracks()`):
- `insertNext()` ("add to queue"): inserta después del track actual Y
  después de cualquier insert anterior aún pendiente
  (`pendingQueuedCount`, decrementado al avanzar la reproducción) -
  FIFO como la cola real de las apps; insertar siempre en index+1
  habría reproducido agregados consecutivos en orden inverso. Poda y
  re-encola los preloads solo desde el punto de inserción si cae
  dentro de la ventana de precarga.
- `replaceUpcoming()` ("set queue"): `currentTracks` pasa a ser
  prev_tracks + [actual] + next_tracks con el índice en [actual],
  conservando la cabeza de `preloadedTracks` (mismo patrón
  keep-the-head del branch no-initial de `updateTracks()`).
- Parseo: `ContextResolver::trackFromJson()` (extraído del parseo de
  páginas, ahora compartido) - `command.track` para add_to_queue,
  `command.prev_tracks`/`next_tracks` para set_queue, mismo mapeo
  JSON canónico de ContextTrack en los tres lugares.
- Semántica "nada sonando": warn + `success=true` (imita el branch
  nil-tracks de go-librespot, que también acepta en silencio).

**Limitación conocida y aceptada**: nuestros PUTs no reportan
`PlayerState.next_tracks`/`prev_tracks` (schema recortado), así que la
VISTA de cola en la app puede no reflejar el estado real del device
aunque la reproducción sí siga el orden editado. Si en hardware
resulta confuso, el paso siguiente sería reportar next_tracks (schema
+ encoding con callbacks nanopb, costo real de RAM/flash - decisión
empírica).

Build limpio (50% libre), suite host 107/107 + TSan limpios.
Pendiente hardware: (a) medir si la primera reproducción baja de ~7s,
(b) agregar canciones a la cola desde la app mientras suena y
verificar que suenan en orden FIFO, (c) que el "siguiente" natural
tras editar la cola encadena bien.

---

## 13. Recorte de MercurySession: fuera la maquinaria de suscripciones (2026-07-15)

Pregunta del usuario: ¿ya se puede eliminar `MercurySession`? Respuesta
tras mapear todos los usos y verificar contra go-librespot: **no entera
- son dos capas fusionadas, y una es imborrable**:

1. **Capa de sesión AP** (TCP + shannon contra el access point):
   `authenticate()` (credenciales reutilizables al parear),
   `requestAudioKey()` (claves AES de audio - NO existe endpoint HTTPS
   para esto; go-librespot mantiene la misma conexión binaria,
   `ap/ap.go` `PacketTypeRequestKey`→`PacketTypeAesKey`), country code
   (cmd 27) y time sync (cmd 4). Se queda.
2. **Capa de protocolo Mercury** (requests `hm://` + SUB/UNSUB): tras
   borrar SPIRC, las suscripciones quedaron sin ningún usuario
   (`SpircHandler::subscribeToMercury()` era el único), y de los
   requests solo sobrevive el GET de metadata de `TrackQueue`.

**Paso 1 ejecutado** (elegido por el usuario): borrada la maquinaria
de suscripciones completa - `executeSubscription()` (renombrada a
`execute()`, ahora la implementación real, sin parámetro
`subscription`), el mapa `subscriptions`, los valores SUB/UNSUB/SUBRES
del enum y sus cases en `handlePacket()`, la parte de suscripciones de
`failAllPending()`, y de paso `setConnectedHandler()`/
`connectionReadyCallback`/`executeEstabilishedCallback` (su único
usuario también era SpircHandler, para suscribirse al conectar). El
test F93 (TSan) se adaptó: su case de `executeSubscription(SUB,...)`
pasó a ejercitar el overload de `execute()` con payload (el camino de
encoding de DataParts, que sigue siendo distinto del GET sin parts del
otro case) - misma cobertura de concurrencia sobre los mutexes que
quedan.

**Paso 2, diferido como decisión aparte**: migrar la metadata de
Mercury GET a spclient HTTPS (`/metadata/4/track/`, como go-librespot)
mataría el último request Mercury y permitiría extirpar la capa
entera (decode de responses, sequence ids, callbacks), dejando una
sesión AP magra. No es mejora de velocidad (Mercury va multiplexado
sobre la conexión ya abierta, sin TLS extra) - es eliminación de un
protocolo custom a cambio de tocar el camino caliente de carga de
CADA track, con la re-validación en hardware que eso implica.

Verificación: firmware limpio (50% libre), suite host 107/107 + TSan
limpios (f93 con la operación adaptada, f87 intacto).

---

## 14. Transfer en pausa: icono play/pause incorrecto en el display (2026-07-15)

Escenario: cliente de PC parado en una playlist SIN reproducir, se
selecciona el device ESP32. El transfer llega con un contexto real y
`TransferState.Playback.is_paused=true` / `playback_speed=0.0`
(decodificado a mano del payload de 912 bytes). El motor hace lo
correcto: `handleTransfer()` lee `is_paused` → `loadTracks(startPaused=
true)` → `TrackPlayer` decodifica pero `pcmWrite()` devuelve 0 mientras
`paused`, así que NO sale audio (el `TrackPlayer:245 Playing` del log es
solo el loop de decode arrancando, no salida real). Correcto y
alineado con go-librespot (`pause := transferState.Playback.IsPaused
&& ...`).

**Bug, puramente de UI** (`components/ui/player_screen.cpp`):
`apply_track_info_locked()` forzaba `s_is_playing = true` +
`set_play_pause_icon_locked(true)` en CADA `TRACK_INFO`. El orden de
eventos que llega a la UI en una carga de track es `PLAY_PAUSE` (desde
`trackLoadedCallback`, antes de que fluya audio) y DESPUÉS `TRACK_INFO`
(desde `pcmWrite`→`notifyAudioReachedPlayback`) - así que el
`TRACK_INFO` pisaba el estado de pausa que el `PLAY_PAUSE` acababa de
setear bien, dejando el icono de pausa (= "reproduciendo") sobre un
transfer que estaba en pausa. Era una asunción válida en la era SPIRC
(los transfers siempre arrancaban reproduciendo); con connect-state
honrando `is_paused`, un track puede cargar en pausa.

**Fix (Opción A, elegida sobre "simplemente reproducir como main"):**
`apply_track_info_locked()` ahora refleja el `s_is_playing` actual en
vez de forzar `true` - actualiza labels/carátula/duración pero deja el
icono a lo que dijo el último PLAY/PAUSE. Como el `PLAY_PAUSE` siempre
precede al `TRACK_INFO` de una misma carga de track, `s_is_playing` ya
está correcto en ese punto. Solo UI, no toca el motor. El device queda
cargado en pausa listo para darle play (local o remoto), como un
Connect real.

**No manejado (anotado, edge case):** `command.options.restore_paused
== "resume"` - el caso donde el cliente pide explícitamente resumir al
transferir (go-librespot lo chequea). Honrar `is_paused` es el 90%
correcto; se agrega si aparece la necesidad real.

Build limpio, 50% libre. De paso se corrigieron dos comentarios stale
de `player_screen.cpp` que referenciaban `spircMutex` (ya inexistente).

---

## 15. Transfer con solo current_track (sin context ni queue) (2026-07-15)

Escenario: parado en una playlist en el cliente de PC, se selecciona el
device y NADA aparece en pantalla (ni track ni nada para reproducir).
Log: `transfer: no context/queue tracks in TransferState, becoming
active with an empty queue`.

Decodificado a mano el `TransferState` de 163 bytes: `context.uri` está
presente pero VACÍO (longitud 0), `queue.tracks` vacío (0 tracks), PERO
`playback.current_track` trae un track real - uri
`spotify:track:4EchqUKQ3qAQuRNKmeIpnf` + gid de 16 bytes. Es una tercera
forma de transfer que el código no contemplaba: `handleTransfer()` solo
miraba context y queue, así que caía en la rama "nada que cargar" (§10)
e ignoraba el `current_track` que sí tenía la info.

Esta forma se distingue de la "genuinamente vacía" de §10 (device
seleccionado sin nada sonando en ningún lado) justamente por
`has_current_track`: aquella no lo tenía (nada suena → no hay track
actual), esta sí.

**Fix**: nueva rama en `handleTransfer()` entre la de queue y la de
"vacío" - si no hay context resoluble ni queue pero
`playback.has_current_track`, carga una cola de UN solo track construido
directo del `current_track` (uri + gid, sin HTTP). Limitación aceptada:
el contexto completo de la playlist no es recuperable de este mensaje
(el uri que necesitaríamos para context-resolve es el vacío), así que
"siguiente" no continúa la playlist - es lo máximo que se puede con lo
que el transfer realmente entregó. De paso se extrajo el helper
`contextTrackToRef()` (ContextTrack protobuf → TrackReference),
compartido por las ramas de queue y current_track (gemelo wire-protobuf
del `ContextResolver::trackFromJson()` que hace lo mismo para el
ContextTrack JSON de las respuestas HTTP). Sin regresión en el caso
§10: aquel tiene `has_current_track=false`, cae en la rama vacía igual
que antes.

Build limpio, 50% libre. Pendiente confirmar en hardware que este
transfer ahora carga y muestra el track (en pausa, listo para play,
por el fix de §14).

---

## 16. Botón de resume deshabilitado en el cliente mientras el ESP32 está pausado (2026-07-15)

Escenario reportado: siempre que el ESP32 está en pausa (sin importar
cómo llegó a ese estado - transfer pausado, o pausa disparada desde el
mismo cliente), el botón de play/resume queda deshabilitado en el
cliente de PC. Solo se puede reanudar tocando play en la pantalla del
ESP32 - recién ahí el cliente se "entera" y vuelve a habilitar sus
propios controles. El botón de pausa (mientras suena) sí funciona
siempre.

**Hipótesis, no confirmación por bytes** (a diferencia de los hallazgos
anteriores de esta sesión): comparando campo por campo nuestro
`PlayerState` contra lo que `go-librespot` siempre popula
(`daemon/controls.go`), encontramos que NUNCA seteábamos
`session_id`, `playback_id` ni `context_uri` - aunque los tres ya
existían en nuestro schema recortado (`connectstate.proto`, con
tamaños ya reservados en `.options`: session_id/playback_id max 64,
context_uri max 150) desde que se armó por primera vez. go-librespot
los popula siempre: `SessionId` (random 16 bytes, o el
`original_session_id` de un transfer) al activarse, `PlaybackId`
cuando un stream de audio realmente abre, `ContextUri` del contexto
resuelto. Sin un identificador de sesión/playback, es plausible que el
cliente no confíe lo suficiente en el estado publicado como para
enviar un comando "resume" que necesita correlacionar con ESA sesión -
mientras que "pause" (una acción que solo depende de ver is_playing=
true) sí se ofrece sin problema.

**Fix**: `PlayerEngine` ahora genera y publica los tres campos:
- `sessionId`: 16 bytes random (vía `Crypto::generateVectorWithRandomData`,
  ya usado en el proyecto para nonces/websocket keys), base64,
  generado una vez en el constructor (vida útil de toda la sesión del
  device) - enviado en TODOS los PUTs (`putState()` y el de
  `runTask()`).
- `playbackId`: regenerado en `notifyAudioReachedPlayback()` cada vez
  que arranca un track real - misma granularidad que el
  `primaryStream.PlaybackId` de go (por-stream-abierto).
- `contextUri`: seteado en `handleTransfer()`/`handlePlay()` cuando
  resuelven un contexto real (`haveContext`); no se toca en los casos
  sin contexto resoluble (§10 vacío, §15 solo-current_track) - no hay
  evidencia de que esos casos estén rotos por este campo.

Build limpio, 50% flash libre sin cambios (los campos ya estaban en
el schema). Suite host 107/107 + TSan limpios. **Pendiente confirmar
en hardware** que esto es realmente la causa - a diferencia de otros
hallazgos de esta sesión, no se pudo verificar leyendo el wire (no hay
forma de inspeccionar qué chequea internamente el cliente de escritorio
antes de habilitar un botón). Si el botón sigue deshabilitado tras
este fix, la causa es otra y hay que seguir buscando con logs nuevos.

---

## 17. `update_context` respondía success=false (candidato real al botón deshabilitado) (2026-07-15)

El fix de §16 (session_id/playback_id/context_uri) no resolvió el botón
de resume deshabilitado. Nuevo log del mismo escenario mostró algo que
no habíamos visto antes: justo después de un `pause` exitoso (~3s
después), el cliente manda un comando `update_context`, y como no
estaba implementado, contestamos `success=false`.

Investigado contra ambas referencias: `update_context` NO es una carga
de cola/contexto nueva (eso es `transfer`/`play`) - es un sync liviano
de metadata/restricciones del contexto actual. Ambas implementaciones
lo tratan como **nunca-falla**:
- go-librespot (`daemon/player.go` `case "update_context":`): si
  `command.context.uri` no coincide con el contexto actual, solo
  loguea un warning y devuelve `nil` (éxito) sin aplicar nada. Si
  coincide, actualiza `ContextRestrictions`/`ContextMetadata` y hace un
  PUT normal.
- librespot-rust (`spirc.rs` `UpdateContext`): mismo criterio - ignora
  silenciosamente si el uri no es el actual, si no encola un
  re-resolve.

Contestar `success=false` (como "no implementado") diverge de las dos
referencias, que jamás fallan este comando. Es un candidato real: el
patrón temporal (falla justo después de cada pause) es coherente con
que el cliente, al ver un comando propio rechazado, deje de confiar en
el estado del device lo suficiente como para ofrecer controles que
dependen de esa confianza (resume).

**Fix**: `update_context` ahora responde éxito siempre, sin aplicar
nada (no tenemos campos de metadata/restricciones de contexto en
nuestro `PlayerState` recortado - no hay nada que aplicar, pero sí hay
que dejar de fallar el ack). Mismo criterio "nunca-falla" que las dos
referencias.

Build limpio, 50% flash libre sin cambios. Pendiente confirmar en
hardware si esto sí resuelve el botón de resume - si no, el candidato
sigue siendo el trío session_id/playback_id/context_uri de §16 (o
alguna combinación), y hay que seguir descartando con logs nuevos.

---

## 18. Botón de resume sigue deshabilitado tras §16/§17 - agregado log de PUT completo (2026-07-15)

Ni el eco de session_id/playback_id/context_uri (§16) ni el fix de
update_context (§17) resolvieron el botón. Nuevo log: esta vez el
usuario pausó DESDE el cliente (no desde la placa), el comando
`pause` tuvo éxito, el PUT salió bien - y el botón quedó igual de
griseado, sin que `update_context` ni ningún otro comando apareciera
después. Consistente entre ambas pruebas: el botón se deshabilita
justo después del PUT de pausa, sin importar qué lo precedió - lo que
descarta `update_context` como causa (no apareció esta vez) y deja
como sospechoso el contenido mismo de ESE PUT.

En vez de seguir comparando campo por campo contra las referencias sin
ver qué mandamos realmente, se agregó un log que imprime el
`PlayerState` completo justo antes de cada PUT real (`runTask()`):
`is_playing`, `is_paused`, `track`, `position`, `duration`,
`session_id`, `playback_id`, `context_uri`. Mismo principio que ya dio
resultado con el hex dump de TransferState en §10-§15: terreno firme
en vez de inferencia. Build limpio, 50% flash libre sin cambios.

Pendiente: repetir el escenario (reproducir → pausar desde el cliente)
y mandar el log nuevo con la línea `connect-state PUT: is_playing=...`
- con eso se compara byte a byte contra lo que las referencias
publican y se cierra esta investigación con datos, no hipótesis.

---

## 19. CAUSA RAÍZ ENCONTRADA: is_playing/is_paused tratados como mutuamente excluyentes (2026-07-15)

Con §16/§17 sin resolver el botón, el usuario pidió comparar puntualmente
contra rust librespot y sus protobuf/campos. El nuevo log con el diag de
§18 dio evidencia dura primero: los tres PUTs capturados (pausado-antes-
de-play, reproduciendo, pausado-tras-click-en-cliente) son idénticos en
`track`/`session_id`/`playback_id`/`context_uri` - la ÚNICA variable que
cambia junto con el botón es `is_playing`/`is_paused` mismos. Eso apuntó
directo al campo, no a algo faltante alrededor.

Leyendo `connect/src/state.rs` de librespot-rust (`set_status()`) apareció
la causa, con un comentario que nombra el síntoma exacto:

```rust
// desktop and mobile require all 'states' set to true, when we are paused,
// otherwise the play button (desktop) is grayed out or the preview (mobile) can't be opened
player.is_buffering = player.is_paused || ...;
player.is_playing = player.is_paused || ...;   // is_playing sigue TRUE al pausar
```

Confirmado también en go-librespot (`daemon/controls.go`,
`case player.EventTypePause:`):
```go
p.state.player.IsPlaying = true   // también sigue TRUE
p.state.setPaused(true)
```

**Las dos referencias tratan `is_playing`/`is_paused` como campos
independientes, no opuestos.** `is_playing` significa "hay una sesión
activa/cargada" y se mantiene TRUE durante todo el ciclo play↔pause -
solo pasa a `false` cuando genuinamente no hay nada cargado (stop/
desconexión). `is_paused` (+ `playback_speed=0`) es lo que realmente
indica si el audio fluye. Nosotros hacíamos `is_playing = !is_paused`
- exactamente la reproducción del bug que el comentario de rust describe
palabra por palabra.

**Fix**: en el PUT real (`runTask()`), `ps.is_playing` ahora es
`!trackUri.empty()` (hay sesión cargada) en vez de `isPlaying` (que
ahora solo alimenta `is_paused`/`playback_speed`); se agrega
`ps.is_buffering = ps.is_paused` (mismo patrón "todo en true al pausar"
que ambas referencias documentan). `putState()` (registro/become-active
vacío, sin track) queda sin tocar - ahí `is_playing=false` por default
sigue siendo correcto (no hay sesión cargada).

Build limpio, 50% flash libre sin cambios. Suite host 107/107 + TSan
limpios. Pendiente confirmar en hardware - por primera vez en esta
investigación con una causa raíz verificada en código real (no
hipótesis), no solo "los datos no contradicen esto".

**CONFIRMADO en hardware (2026-07-15)**: era exactamente eso. El botón
de resume del cliente queda habilitado correctamente.

**Limpieza post-fix**: eliminado el log de diagnóstico de §18 (su
comentario ya decía "remove once that's closed" - cerrado); removidos
los locals `loggedPlaybackId`/`loggedContextUri` que solo existían para
alimentar ese log. Recortados los comentarios de §16/§17 que
enmarcaban session_id/playback_id/context_uri y el fix de
`update_context` como "candidatos" a la causa del botón - **ambos
cambios se mantienen** (son comportamiento correcto verificado contra
las dos referencias, independientemente de que no hayan sido la causa
real), solo se corrigió el texto para no atribuirles una causalidad
que resultó ser de `is_playing`/`is_paused`. De paso, uniformado el
scope del lock en `handleTransfer()`'s branch de contexto (llaves
explícitas, igual que en `handlePlay()`) y corregido un warning
preexistente de variable sin usar en `putStateInactive()` (de §11).
Build limpio sin warnings, suite host 107/107 + TSan limpios.

---

## 20. Latencia de primera reproducción: TCP_NODELAY + TLS 1.3 (2026-07-15)

Pedido explícito del usuario: revisar si la implementación de WebSocket
es la apropiada y si hay ganancias de más bajo nivel disponibles vía
`bell`. Investigación (no implementación a ciegas) antes de tocar nada:

- **`esp_websocket_client`** (Dealer WS): config revisada
  (`EspWebSocketTransport.cpp`) - usa `esp_crt_bundle_attach` (igual que
  el resto del proyecto), reconexión propia deshabilitada a propósito
  (`DealerClient` la maneja con token fresco, §6.5), reassembly con el
  tope de 256KB de §11. Apropiada para el rol que cumple - es un costo
  de una sola vez por sesión (no por track), no el cuello de botella real.
- **`bell::TLSSocket`** (la capa TLS compartida detrás de TODO
  `HTTPClient`: Login5 ×3 hosts, PUTs de `PlayerEngine`,
  `ContextResolver`, `CDNAudioFile`) - acá apareció un hallazgo real, no
  hipotético: **`PlainConnection.cpp`** (la conexión AP/Mercury) YA
  setea `TCP_NODELAY` explícitamente, pero `TLSSocket.cpp` nunca lo
  adoptó - un patrón ya probado en este mismo repo, simplemente no
  llevado a la capa TLS más nueva. Sin esto, el algoritmo de Nagle
  demora cada escritura chica (los propios flights del handshake TLS,
  después cada request/response corto de HTTPClient) esperando más
  datos salientes que nunca llegan en este tipo de intercambio -
  decenas a ~200ms por round-trip, en cada uno de ellos.
- **TLS 1.3**: disponible en este mbedTLS/ESP-IDF
  (`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3`) pero solo TLS 1.2 estaba activo.
  1.2 necesita 2-RTT de handshake, 1.3 necesita 1-RTT. La cadena de
  primera reproducción hace ~5-6 handshakes TLS frescos a hosts
  distintos (accounts.spotify.com, clienttoken.spotify.com,
  login5.spotify.com, spclient, CDN) - cada uno se ahorra un
  round-trip. Verificado que el CDN (`CDNAudioFile.cpp`) ya reusa
  conexión dentro de un mismo track (header+footer+chunks,
  `reused=1` en los logs) - lo que NO hace es reusar entre tracks
  distintos (cada `QueuedTrack` tiene su propio objeto `CDNAudioFile`,
  con su propia conexión) - anotado como oportunidad aparte, más
  invasiva (movería el dueño de la conexión a algo de vida más larga),
  diferida a pedido del usuario.

**Fix**: (1) `TLSSocket::open()` ahora setea `TCP_NODELAY` justo
después del `mbedtls_net_connect()`, antes del handshake - beneficia
handshake y todos los reads/writes posteriores de esa conexión. (2)
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` agregado a `sdkconfig.defaults`
(TLS 1.2 se mantiene activo también - mbedTLS negocia la versión más
alta en común y cae a 1.2 solo si el servidor no soporta 1.3, así que
es seguro activarlo especulativamente). Requirió `idf.py reconfigure`
(el `sdkconfig` ya generado tenía la opción resuelta explícitamente en
"not set" - un `sdkconfig.defaults` nuevo no se re-aplica solo en un
build incremental).

Build limpio, flash bajó de 50% a 49% libre (esperable - TLS 1.3 agrega
X25519/HKDF al binario), margen sano. Suite host 107/107 + TSan
limpios (ninguno de los dos cambios toca código compilado para host).
Pendiente medir en hardware si la cadena de primera reproducción baja
de los ~7s observados.

**TLS 1.3 REVERTIDO en hardware (2026-07-15)**: no fue "seguro activarlo
especulativamente" como se asumió. La primera prueba en hardware dio
regresión real - la sesión fallaba consistentemente (dos intentos, mismo
resultado) con `session failed: Connection closed while reading HTTP
response`, justo después de que el certificado se validara con éxito.
No investigado más a fondo si es una incompatibilidad genuina con el
lado servidor o un bug de esta versión específica de ESP-IDF/mbedTLS
4.0 para TLS 1.3 cliente - dado que `TCP_NODELAY` es un cambio mucho
más chico y aislado (una opción de socket, no toca el protocolo en
absoluto), se revirtió solo TLS 1.3 para aislar la variable en vez de
revertir los dos a ciegas.

Revertido: `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3` volvió a "not set" -
directamente en `sdkconfig` (un `idf.py reconfigure` con el default
comentado en `sdkconfig.defaults` NO alcanza para revertir una opción
que el `sdkconfig` ya generado tiene resuelta explícitamente en `=y` -
mismo problema, en sentido inverso, que activarla la primera vez).
`sdkconfig.defaults` deja la línea como comentario explícito (no
borrada del todo) documentando que se probó y no funcionó, para que no
se vuelva a intentar sin releer esto. `TCP_NODELAY` en `TLSSocket.cpp`
se mantiene - build limpio, flash de vuelta a 50% libre. Pendiente
confirmar en hardware que la sesión conecta normal de nuevo con
`TCP_NODELAY` solo, y medir si por sí solo aporta alguna mejora medible
en la latencia de primera reproducción.

**CONFIRMADO en hardware (2026-07-15), solo `TCP_NODELAY`**: sesión
conecta normal, sin errores, cola precargando bien. Latencia "transfer
decodificado → audio real": ~6.4s (19.762→26.145 en el log), contra
los ~7-9s de baseline - mejora real pero modesta, sin A/B limpio en
las mismas condiciones de red para aislarla con precisión. Señal más
sólida: los `CDN fetch` de esta corrida quedaron muy consistentes
(401-643ms, banda angosta) sin los outliers grandes que aparecían
antes (927ms, 1728ms, hasta 2743ms en corridas previas) - coincide con
lo esperado de eliminar la demora de Nagle en intercambios request/
response cortos: no baja el piso, recorta la cola de casos lentos.
Una sola corrida no es prueba estadística, pero no hay señal negativa.
`is_playing`/`is_paused` (§19) siguen correctos en este log también
(PUTs reason 4 con track real, sin regresión).

---

## 21. CDN: reusar conexión entre tracks distintos, no solo dentro de uno (2026-07-15)

La tercera oportunidad de la investigación de latencia (§20), implementada
a pedido explícito del usuario.

**Diseño**: `CDNConnection` (struct nuevo, `CDNAudioFile.h`) - un
`unique_ptr<HTTPClient::Response>` + el host al que está conectado +
cuándo se usó por última vez. Antes vivía como `httpConnection` dentro de
cada `CDNAudioFile`, un objeto por track (`QueuedTrack::getAudioFile()`
crea uno nuevo cada vez) - ahora `TrackPlayer` es dueño de UNA instancia
para toda la sesión, y se la pasa por referencia a cada `CDNAudioFile`
nuevo. Seguro por diseño existente, no por suerte: `TrackPlayer` procesa
tracks estrictamente uno a la vez (`TrackQueue` precarga metadata/
audio-key/URL de CDN por adelantado, pero la conexión HTTP real solo se
abre cuando `TrackPlayer` decodifica ESE track) - nunca hay dos tracks
pidiéndole al CDN al mismo tiempo, verificado leyendo el código antes de
tocar nada.

**Se centralizó lógica que antes solo existía en `readBytes()`** (reuso-
o-reconexión con detección de conexión posiblemente muerta) en un nuevo
método privado `fetchRange()`, usado ahora por los TRES puntos que piden
al CDN (`openStream()`'s fetch de header, su fetch de footer, y
`readBytes()`) - antes `openStream()` pagaba conexión fresca
incondicionalmente, sin ni siquiera intentar reusar.

**Gap real cerrado de paso**: `HTTPClient::Response::get()`/
`rawRequest()` solo reconecta si el socket está "muerto" - nunca
verificó si la URL nueva apunta a un host distinto del que el socket
realmente tiene abierto. Esto ya era cierto HOY para el reuso dentro de
un track (nunca se disparó en la práctica - todos los tracks de esta
sesión completa usaron el mismo `audio-ak.spotifycdn.com`), pero al
extender el reuso a través de tracks distintos - con más tiempo entre
pedidos, más chance de que el CDN reasigne un borde distinto - se volvió
más prudente no asumirlo. `fetchRange()` ahora compara el host antes de
confiar en un reuso.

**Tests actualizados**: los 5 casos existentes de `cdn_audio_file_test.cpp`
(F58/F82/F86, contra un servidor HTTP fake local) se adaptaron a la
firma nueva del constructor. Se agregó un caso nuevo, específico para
esto: dos `CDNAudioFile` distintos (representando dos tracks
consecutivos) compartiendo un mismo `CDNConnection`, verificando con
`server.connectionCount() == 1` que efectivamente se abre una sola
conexión TCP real para las dos, no dos.

Build de firmware limpio (50% libre, sin cambio - es reorganización de
código existente, no código nuevo de peso). Suite host: 27 casos/112
asserts (antes 26/107), TSan limpio en f93/f87. Pendiente confirmar en
hardware: que tracks consecutivos de una playlist muestren `reused=1`
en el `CDN fetch` incluso en la PRIMERA fetch de cada track nuevo (no
solo dentro del mismo track como hasta ahora), y que no haya
regresiones de audio (cortes, decodificación incorrecta) al encadenar
varios tracks reales.

**Confirmado en hardware (2026-07-15)**: tracks consecutivos de una
playlist real muestran `headerReused=1`/`reused=1` desde el segundo
track en adelante, transiciones perceptiblemente más rápidas y
fluidas, sin cortes de audio. Cierre de §21.

---

## 22. WebSocket propio sobre `bell::TLSSocket`, reemplazando `esp_websocket_client` (2026-07-15)

Disparado por dos eventos reales en la misma sesión de hardware que
confirmó §21:

1. **Reconexión silenciosa del Dealer en pleno playback.** El cliente
   de escritorio cambió de playlist; el log mostró
   `transport_ws: esp_transport_ws_poll_connection_closed: unexpected
   data readable on socket=58` seguido de `websocket_client: Connection
   terminated while waiting for clean TCP close`. El comando `play` que
   venía en camino sí se procesó (`DealerClient.cpp:366`,
   `success=1`) pero la respuesta no se pudo enviar
   (`DealerClient.cpp:406: failed sending reply`) porque el socket ya
   estaba muerto; `DealerClient` reconectó solo (backoff ya existente)
   y el PUT de estado que sigue a la reconexión resincronizó todo -
   el sistema se auto-sanó, pero la causa raíz vivía dentro de
   `esp_websocket_client`, fuera de nuestro control y sin visibilidad.
2. **`esp-tls: Failed to create socket`** rompiendo el fetch de
   carátula tras un `next` local, después de escuchar un rato. Analizado
   por separado (agotamiento de `CONFIG_LWIP_MAX_SOCKETS=10`: el
   `httpd` de zeroconf reserva ~3 sockets internos
   (`HTTPD_DEFAULT_CONFIG().max_open_sockets=7`, según el propio header
   de `esp_http_server.h`) más 5 conexiones persistentes (AP/Mercury,
   Dealer WS, PUT de connect-state, ContextResolver, CDN de §21) dejan
   solo ~2 de margen para lo transitorio - Login5 + cover art
   compitiendo a la vez agotó ese margen). No es lo que motivó este
   cambio de WebSocket en sí, pero confirma que el presupuesto de
   sockets del dispositivo es ajustado y cualquier conexión de más
   pesa.

El usuario preguntó explícitamente si convenía un WebSocket propio
sobre clases de `bell` + el stack de lwIP en vez de depender de
`esp_websocket_client`, dado que ya existía casi todo lo necesario:
`PortableWebSocketTransport.cpp` (usado hoy solo en tests de host) es
un cliente RFC 6455 completo, ya probado (`ws_transport_echo_test.cpp`,
handshake + longitudes 7/16/64 bits + ping/pong + close), construido
sobre `bell::SocketStream` → `bell::TLSSocket` - el mismo socket que
ya usan Mercury/HTTPClient/CDN y que ya tiene TCP_NODELAY (§20).

**Por qué no simplemente compilar `PortableWebSocketTransport.cpp`
también en ESP32**: su diseño (hilo lector dedicado + cola +
`std::iostream` sobre `SocketStream`) tiene dos problemas para
producción en el dispositivo:
- `SocketStream` marca `failbit` en cualquier lectura corta y no
  distingue "no llegó nada todavía, normal" de "la conexión está
  muerta" - justo la distinción que hace falta para tolerar que el
  Dealer esté 30s+ sin mandar nada (comportamiento normal, no un
  fallo).
- Un hilo separado leyendo mientras el hilo de `DealerClient` escribe
  respuestas implica dos hilos tocando el mismo `mbedtls_ssl_context`
  concurrentemente. mbedTLS no documenta esto como seguro por defecto
  (no hay mención de thread-safety para lectura/escritura concurrente
  en `ssl.h`); asumirlo sin verificarlo, o meter un mutex sin estar
  seguro de qué protege, es exactamente el tipo de bug sutil que
  podría aparecer solo esporádicamente - inaceptable en la conexión
  que se está intentando hacer más confiable.

**Diseño implementado** (`EspWebSocketTransport.cpp`, reescrito
completo, mismo nombre de archivo así el CMake no cambia):

- **Un solo hilo, cero mutex.** Todo el I/O (lecturas Y escrituras)
  ocurre en el hilo de `DealerClient::runTask()` - se evita por
  construcción la pregunta de si mbedTLS tolera lectura/escritura
  concurrente, en vez de asumir que sí.
- **`bell::TLSSocket` extendido** (no tocado `bell::SocketStream`,
  para no heredar su semántica de `failbit`): timeout de lectura ahora
  configurable por instancia vía `setReadTimeout()` (antes, constante
  global de 15000ms fija para TODO uso de `TLSSocket`, F58) y un nuevo
  `lastReadTimedOut()` que distingue "no llegó nada en el timeout, no
  necesariamente fatal" (`MBEDTLS_ERR_SSL_TIMEOUT`) de "error real"
  (cualquier otro código negativo - reset, close-notify, etc.). Cambio
  aditivo y retrocompatible: todo caller existente (Login5, PUT de
  connect-state, ContextResolver, CDN) nunca llama a
  `setReadTimeout()`, conserva los 15000ms y el comportamiento de F58
  intactos.
- **`TLSSocket::close()` ahora manda `mbedtls_ssl_close_notify()`**
  antes de liberar el socket (antes: cierre TCP crudo sin aviso TLS -
  gap preexistente, cerrado de paso, beneficia a TODOS los usos de
  `TLSSocket`, no solo al WebSocket).
- **`receiveMessage(msg, timeoutMs)`**: espera el primer byte del
  próximo frame respetando el `timeoutMs` corto del caller
  (`RECEIVE_POLL_MS=1000` en `DealerClient`) - si no llega nada y fue
  timeout (no error), es un "poll normal", no fatal. En cuanto llega
  el primer byte, se compromete a terminar ESE frame (y los de
  continuación de un mismo mensaje fragmentado) con un presupuesto más
  largo y fijo (`FRAME_READ_TIMEOUT_MS=10000`) - abandonar a mitad de
  frame desincroniza el framing igual, así que no tiene sentido
  respetar el timeout corto ahí.
- **Ping/pong propio como reemplazo del keepalive que traía
  `esp_websocket_client`** (`ping_interval_sec` default 10s, que se
  perdía al dejar de usar esa librería): tras 15s sin ningún byte del
  servidor, manda un ping; si pasan otros 15s sin ninguna respuesta
  (ni pong ni tráfico real), da la conexión por muerta y la cierra -
  `DealerClient` ve `isConnected()==false` y dispara su backoff de
  reconexión existente, sin cambios en `DealerClient.cpp`. Detección de
  conexión muerta en ~30s en el peor caso, más rápido y más acotado que
  esperar a que TCP/lwIP se de cuenta por sus propios medios (mucho más
  lento e impredecible para un NAT que descarta el mapeo en silencio).
- Reasamblado de fragmentos, chequeo de host, cap de 256KB
  (`MAX_MESSAGE_SIZE`, mismo valor que tenía la versión anterior, F11)
  y manejo de frames de control preservados con el mismo comportamiento
  ya validado en `PortableWebSocketTransport` (frames enmascarados del
  servidor rechazados por RFC 6455, mensajes sobredimensionados
  descartados sin cortar la conexión, binario inesperado descartado con
  log).
- **Limitación conocida y documentada, no un bug**: un mensaje binario
  fragmentado (`FIN=0`) se manejaría mal (el primer fragmento se
  descarta pero un frame de continuación posterior se agregaría al
  buffer de texto) - aceptado explícitamente porque el Dealer solo
  manda JSON en frames de texto, tanto en la práctica como documentado
  en el propio código.

**Dependencias retiradas**: `espressif/esp_websocket_client` fuera de
`idf_component.yml`, `espressif__esp_websocket_client` fuera de
`PRIV_REQUIRES`/`target_link_libraries` en
`components/cspot/CMakeLists.txt`. `PortableWebSocketTransport.cpp`
(rama host, usada en tests) queda intacta - no se tocó, para no
arriesgar su cobertura de test existente.

**Verificado**:
- `idf.py build` limpio, 0 errores, 50% de flash libre (sin cambio
  significativo - se retira una librería vendorizada y se agrega
  código propio de tamaño comparable).
- Suite de host: 27/27 casos, 112/112 asserts (sin cambio - el código
  tocado es exclusivamente `#ifdef ESP_PLATFORM`, no compila en host;
  el stub `TLSSocketStub.cpp` no necesitó tocarse porque
  `setReadTimeout()`/`lastReadTimedOut()` nunca se llaman desde código
  de host).
- `ws_transport_echo_test`: 8/8 casos (handshake, longitudes 7/16/64
  bits en ambas direcciones, ping/pong, close limpio) - valida
  `PortableWebSocketTransport`, la referencia de comportamiento sobre
  la que se modeló la lógica de `EspWebSocketTransport` (misma
  semántica de opcodes/masking/reasamblado, aunque el archivo es
  independiente).
- TSan (f93, f87): limpio, sin regresión.

**Pendiente confirmar en hardware**: que el Dealer se mantenga
conectado establemente durante sesiones largas (objetivo directo: que
el escenario de reconexión silenciosa del punto 1 no vuelva a ocurrir,
o que si ocurre, el log ahora explique la causa en vez de un mensaje
opaco de `esp_websocket_client`), que el ping/pong propio no genere
desconexiones espurias durante los períodos de 30s+ de inactividad que
son normales en este canal, y que los comandos remotos (play/pause/
transfer) sigan respondiendo con la misma latencia que antes.

**Confirmado en hardware (2026-07-15)**: sesión real de varios minutos
reproduciendo, cambio de playlist desde el cliente de escritorio -
esta vez el log mostró exactamente el escenario que motivó este
cambio, pero con causa explícita: `EspWebSocketTransport.cpp: Dealer
WebSocket: peer sent WebSocket close, disconnecting`, en vez del
`esp_websocket_client: unexpected data readable... waiting for clean
TCP close` opaco de antes. El servidor mandó un frame de cierre WS
legítimo (probablemente reciclando la conexión al procesar los
comandos `play`+`update_context` del cambio de contexto) - se
respondió el cierre según RFC 6455 y `DealerClient` reconectó solo con
su backoff existente en ~1s, sesión resincronizada sin intervención
manual. Objetivo directo cumplido: ya no es un misterio, el log dice
exactamente qué pasó y por qué. Se observaron `CDN fetch` más lentos
de lo habitual (995-5818ms vs ~400-700ms de baseline) en la ventana de
la reconexión - hipótesis de contención de CPU (TLS por software,
F67) compitiendo entre el handshake del WS nuevo y el fetch de CDN,
no confirmado si causó un glitch audible (usuario no estaba
prestando atención en ese momento específico). Pendiente: confirmar
si esa ventana de latencia de CDN es audible, y observar un período
largo sin cambios de contexto para ver si el ping/pong propio se
dispara alguna vez sin generar desconexiones espurias.

**Segunda confirmación del mismo mecanismo (2026-07-15, tras el revert
de §23)**: con `SO_LINGER` ya revertido, sesión estable reproduciendo
(fetches de CDN cada ~2.3s, `reused=1`, nada erático) durante minutos,
hasta que el usuario le dio play/resume desde el cliente. Log:
`DealerClient.cpp:366: ...endpoint=play -> success=1` seguido 7ms
después de `EspWebSocketTransport.cpp:514: peer sent WebSocket close`
- mismo patrón exacto que el primer caso: un comando entrante
(`play`, acompañado de `TrackQueue.cpp:740: updateTracks(initial)`,
sugiriendo que traía recarga de contexto) precede el cierre por unos
milisegundos. Reconectó en ~1.7s, `connection-id` nuevo,
reproducción sin cortes. Descartado que tenga que ver con expiración
de token (uptime del dispositivo ~15min en ese momento, Login5 dura
3600s; los tokens en las URLs de CDN son por-track, sin relación con
la conexión del Dealer). Confirma que es un único evento limpio y
autocurado, no el loop errático de §23 (que sí estaba resuelto en
esta misma sesión).

**Contraprueba pedida por el usuario, con motivo real**: el otro
proyecto del usuario (`spotify_app`, también Dealer WebSocket sobre
ESP32) no sufre esto - mantiene el mismo `connection_id` siempre. Antes
de aceptar "comportamiento tolerado por diseño" sin más, se investigó
si `spotify_app` era una comparación válida y si había alguna
diferencia arquitectónica real con la referencia (`go-librespot`)
que explicara el cierre:

- **`spotify_app` no es comparable**: revisado su código
  (`spotify_client.c`, `handler_callbacks.c`) - nunca hace el PUT de
  registro (`NEW_DEVICE`/`hm://connect-state/v1/devices`), nunca se
  ofrece como dispositivo controlable. Es un oyente pasivo del estado
  de OTRO dispositivo activo - nunca recibe mensajes `type:"request"`
  (comandos), así que nunca ejecuta el camino recibir-comando→
  responder donde aparece el cierre en `cspot`. No puede dar evidencia
  sobre esto porque no lo ejerce.
- **Comparación directa contra `go-librespot`** (la misma referencia
  usada para el fix de §19), en dos ejes:
  - Formato exacto de la respuesta: su struct `Reply` (`dealer/msg.go`)
    serializa a `{"type":"reply","key":"...","payload":{"success":true}}`
    - idéntico byte a byte a lo que genera `DealerClient::sendReply()`.
    No es un problema de formato.
  - Orden de ejecución: `dealer/recv.go`'s `handleRequest` **también
    bloquea la respuesta hasta que el handler del comando termina
    completo** (`success := <-resp` espera al handler, recién después
    `d.sendReply(...)`) - incluye sus propias llamadas de red
    (resolución de contexto). Misma arquitectura que
    `PlayerEngine::handlePlayerCommand()` acá (`handlePlay()`/
    `handleTransfer()` resuelven contexto vía Mercury antes de
    devolver, `sendReply()` se llama después). No hay una versión
    "responder rápido, procesar después" en la referencia que estemos
    dejando de imitar.
  - Manejo de la desconexión en sí (`dealer.go`): el `recvLoop` de
    `go-librespot` no distingue "se cerró justo después de que
    respondí" de cualquier otra causa - cae al mismo backoff/reconexión
    genérico para cualquier desconexión. La referencia ni siquiera
    tiene el concepto de "esto es sospechoso".

**Conclusión, con evidencia comparativa directa esta vez** (no una
suposición): el formato de la respuesta y el orden de ejecución
coinciden exactamente con la referencia, y la referencia misma trata
este tipo de cierre como una desconexión más, sin lógica especial. Es
razonable seguir sosteniendo que es comportamiento real del backend de
Spotify (reciclado de la conexión del Dealer al procesar ciertos
comandos), no un defecto introducido por esta implementación - pero
esta vez la conclusión está respaldada por comparación de código
real, no repetida de memoria. Sigue sin confirmarse con una captura de
paquetes del lado servidor, que no es posible obtener acá.

**Hipótesis afinada con una prueba del usuario (2026-07-15, mismo
día)**: el usuario probó cambiar de playlist con poca separación
temporal entre acciones (~5s) - la conexión del Dealer **no** se
cerró, a diferencia de los 3 casos anteriores, todos con varios
minutos de reproducción idle antes del comando que disparó el cierre.
Esto en un primer momento parece contradecir "cualquier comando
dispara el cierre" - pero en realidad la afina: el patrón real no es
"comando → cierre", es **"comando después de un rato de inactividad →
cierre; comando inmediato (sin idle previo) → conexión persiste"**.

Encaja con un patrón común en infraestructura de backends a gran
escala: reciclado perezoso de conexiones idle - el servidor no mata
proactivamente una conexión callada (desperdiciaría el trabajo de
mantenerla), pero cuando por fin llega la próxima actividad real
después de un tramo de silencio, aprovecha ese momento para cerrarla y
forzar una reconexión (que de paso puede rebalancear la conexión a
otro backend, redistribuir carga, liberar recursos - todas razones
legítimas del lado servidor para hacerlo justo ahí y no antes). Esto
también es coherente con que el ping/pong propio (§22) nunca disparó
su propio timeout durante estos tramos idle - el servidor sigue
respondiendo pings con normalidad todo ese tiempo, el corte llega
recién con tráfico de aplicación real.

No confirmable sin captura de paquetes del lado servidor (no
disponible acá), pero es la explicación más coherente con **toda** la
evidencia hasta ahora: los 3 cierres observados (todos con idle previo
largo) y la prueba de 5s del usuario (sin idle previo, sin cierre).

**Experimento: ajustar el ping propio para evitar la ventana idle
(2026-07-15, mismo día)**: si el reciclado es por inactividad del lado
servidor, y nuestro ping/pong (cada 15s) ya estaba corriendo sin
prevenir el cierre en los 3 casos observados, dos lecturas posibles:
(a) el timer del servidor que decide "esta conexión está idle" no
cuenta pings/pongs como actividad real, y ningún ajuste de frecuencia
va a cambiar nada, o (b) sí cuenta, pero 15s no alcanzaba a mantenerla
por debajo del umbral que el servidor considera "reciente". No hay
forma de distinguir (a) de (b) sin visibilidad del servidor - pero (b)
es barato de probar y (a) no tiene downside si resulta ser el caso
(seguimos igual de bien que antes). Se ajustó, en
`EspWebSocketTransport.cpp`:

- `PING_INTERVAL_MS`: 15000 → 5000 - la ventana exacta que la propia
  prueba del usuario mostró como segura (un comando ~5s después de
  actividad previa no disparó el cierre).
- `PING_TIMEOUT_MS`: 15000 → 10000 (detección de conexión muerta en el
  peor caso: ~15s en vez de ~30s - más rápido, de paso).
- Log nuevo (`debug`) cada vez que se manda un ping propio, con el
  tiempo idle acumulado - visibilidad para el próximo log de hardware,
  confirma que están saliendo con la cadencia esperada
  independientemente de si terminan evitando el cierre o no.

Build de firmware limpio, 50% de flash libre (sin cambio - solo
constantes). No afecta al build de host (archivo exclusivamente
`#ifdef ESP_PLATFORM`).

**Pendiente confirmar en hardware**: dejar reproducir varios minutos
sin tocar nada, dar un comando (play/pause/next), y ver si el cierre
sigue apareciendo. Si SÍ sigue apareciendo con el ping cada 5s, eso
apunta a la lectura (a) de arriba (el servidor no cuenta los pings como
actividad) - en ese caso no hay más ajuste de cliente posible, y toca
aceptarlo definitivamente como comportamiento del backend fuera de
nuestro control (opción "aceptarlo como tolerado" ya planteada antes).
Si NO vuelve a aparecer, confirma la lectura (b) y el ajuste queda como
mitigación real.

**El experimento se revirtió antes de probarse en hardware, con mejor
evidencia (2026-07-15, mismo día)**: a pedido del usuario, se buscó la
gestión real de la conexión Dealer en librespot-rust
(`core/src/dealer/mod.rs`) en vez de seguir especulando. Encontrado:

```rust
const PING_INTERVAL: Duration = Duration::from_secs(30);
const PING_TIMEOUT: Duration = Duration::from_secs(3);
```

Tres hallazgos que cambian la lectura anterior:
- **Ningún TTL/expiración de `Spotify-Connection-Id`** en todo el
  módulo - ni comentario, ni lógica, ni reconexión programada por
  tiempo. Nada que "prever" del lado cliente.
- **Su ping es cada 30s**, más lento que el 15s original de este
  proyecto, y mucho más lento que el 5s del experimento recién hecho.
  Si pingear más seguido evitara el reciclado del lado servidor, la
  referencia (que pinguea 6x menos seguido) debería sufrirlo más, no
  nunca mencionarlo - la premisa completa del experimento de arriba
  queda debilitada por esto.
- **Manejo de cierre completamente genérico**: cuando el WS se cierra
  (por cualquier motivo), loguean `"Websocket connection closed."` y
  reconectan con delay fijo de 10s - sin distinguir "se cerró justo
  después de responder un comando" de cualquier otra causa. Ningún
  caso especial, ninguna sospecha documentada en su propio código.

**Revertido antes de llegar a probarse en hardware** (no hacía falta
gastar un ciclo de test para confirmar algo que la propia referencia
ya contradice): `PING_INTERVAL_MS` vuelve a 30000 (alineado con la
referencia, no a los 15000 originales), `PING_TIMEOUT_MS` baja a 3000
(alineado con la referencia - más agresivo que los 15000/10000
anteriores, detección de conexión muerta más rápida una vez que se
manda un ping). El log de "sent keepalive ping" (visibilidad) se dejó
tal cual. Build limpio, 50% de flash libre.

**Conclusión, con la evidencia más fuerte reunida hasta ahora**: no
hay nada que ajustar del lado cliente para evitar este cierre - ni
frecuencia de ping, ni nada relacionado a expiración de `connection_id`
(no existe ese concepto en la referencia). Se acepta como
comportamiento del backend de Spotify, tolerado por reconexión
genérica - igual que lo trata la propia librespot-rust.

**Reauditoría completa de `EspWebSocketTransport.cpp` a pedido del
usuario (2026-07-15, mismo día) - dos hallazgos reales, sin relación
con la teoría de arriba**: en vez de seguir mirando el problema desde
el ángulo protocolo/servidor, se releyó el archivo entero buscando
bugs de implementación. Dos aparecieron:

1. **`TLSSocket` nunca tuvo timeout de escritura, solo de lectura.**
   `mbedtls_ssl_write()` → `mbedtls_net_send()` → `send()` crudo
   bloqueante, sin `SO_SNDTIMEO` configurado - a diferencia de
   `PlainConnection.cpp`, que sí lo tiene desde antes. Con el diseño de
   un solo hilo de §22 (deliberado, para no arriesgar mbedTLS con
   lectura/escritura concurrente), un `send()` colgado congelaría la
   tarea entera de `DealerClient` en silencio - sin `fail()`, sin log,
   sin reconexión - un modo de falla peor que cualquiera de los vistos
   hasta ahora, porque no se autocura. Preexistente y compartido por
   TODOS los usuarios de `TLSSocket` (Login5, PUT, ContextResolver,
   CDN), no algo nuevo de §22, pero particularmente peligroso acá.
   **Fix**: `SO_SNDTIMEO=15s` agregado en `TLSSocket::open()`, fijo y
   generoso (no atado al `readTimeoutMs` configurable, que resuelve
   una preocupación distinta - la cadencia de polling del WS).

2. **`PING_TIMEOUT_MS=3000` (copiado de librespot-rust) no estaba
   validado contra el hardware real.** librespot-rust corre en una PC/
   RPi con stack TCP/IP completo y margen de CPU de sobra; este
   dispositivo hace TLS enteramente por software
   (`CONFIG_MBEDTLS_HARDWARE_AES=n`, F67) y ya mostró demoras de varios
   segundos por contención de CPU en operaciones TLS bajo carga (un
   fetch de CDN de 5818ms en un log real, más arriba en este mismo
   documento). Copiar el valor de la referencia sin ajustar por esa
   diferencia de hardware arriesgaba desconexiones propias por falso
   timeout - un dispositivo simplemente falto de CPU un instante,
   confundido con una conexión muerta - que además se hubieran visto
   como un log NUEVO (`no response to keepalive ping`) nunca observado
   hasta ahora, sin haberse probado en hardware todavía. **Fix**: subido
   a 7000ms - mantiene la intención de fallar rápido una vez que
   realmente hay silencio, sin estar ajustado para un hardware que este
   dispositivo no tiene.

**Dos gaps menores de manejo de errores, cerrados de paso**:
`sendText()` y el eco de pong (`case 0x9`) ignoraban un fallo de
escritura - devolvían `false` sin marcar `fail()`, dejando `connected`
en `true` aunque la escritura hubiera fallado de verdad. Antes del fix
de `SO_SNDTIMEO` esto no importaba mucho (el write simplemente se
colgaba, nunca "fallaba" para llegar a este código) - ahora que los
writes SÍ pueden fallar de forma oportuna, ambos casos llaman a
`fail()` con su propio mensaje específico, igual que ya hacía el envío
de ping propio (`onIdlePoll()`) y el eco de close (`case 0x8`, que ya
llamaba `fail()` incondicionalmente después).

Build de firmware limpio, 50% de flash libre. Suite de host: 27/27
casos, 112/112 asserts (sin regresión - los archivos tocados son
`#ifdef ESP_PLATFORM` o, en el caso de `TLSSocket.cpp`, no compilado
en host - se usa `TLSSocketStub.cpp` ahí en su lugar).

**Pendiente confirmar en hardware**: el paquete completo (ping 30s/3s→
30s/7s de referencia + `SO_SNDTIMEO` + detección de fallo de escritura)
sin regresiones - en particular, que 7s de timeout de pong no genere
falsos positivos con jitter real de WiFi, y que el `SO_SNDTIMEO` de 15s
no interfiera con ningún write legítimo pero lento (debería ser
imposible en circunstancias normales, dado el tamaño de los payloads
que maneja este proyecto).

---

## 23. `SO_LINGER` abortivo al cerrar + `LWIP_MAX_SOCKETS` 10→16 (2026-07-15)

Disparado por el usuario, que encontró
[bell PR #48](https://github.com/feelfreelinux/bell/pull/48) (upstream
del que este proyecto vendoriza `cspot`/`bell`) y preguntó si aplicaba
acá. Sí - y llena un hueco de la explicación original del agotamiento de
sockets que rompió `cover_art` (incidente del 2026-07-14, discutido
más arriba en este documento): esa explicación asumía un presupuesto
**estático**
ajustado (~8 de 10 sockets siempre comprometidos en reposo), pero no
explicaba por qué el fallo apareció recién "después de escuchar un
rato" en vez de en la primera transición de track. Un mecanismo de
**acumulación** encaja mejor con esa cronología.

**Lo que dice el PR de bell**: en ESP-IDF v5, encontraron que los PCBs
de TCP no se liberaban correctamente cuando corrían muchas conexiones
- el cierre de socket por defecto en lwIP es *gracioso* (FIN → espera
ACK → `TIME_WAIT`), y ese PCB queda pinneado mientras tanto. Su fix:
`setsockopt(SOL_SOCKET, SO_LINGER, {l_onoff=1, l_linger=0})` fuerza un
**cierre abortivo** (RST en vez de FIN, sin pasar por `TIME_WAIT`),
liberando el recurso de inmediato - requiere `CONFIG_LWIP_SO_LINGER=y`
en Kconfig, sin el cual el `setsockopt` no tiene efecto.

**Verificado en este proyecto (antes del cambio)**: `sdkconfig` tenía
`CONFIG_LWIP_SO_LINGER is not set` (cierre gracioso por defecto) y
`CONFIG_LWIP_MAX_SOCKETS=10` (sin cambios desde el hallazgo original -
la suba a 16 que se había propuesto entonces nunca se llegó a aplicar,
la sesión se desvió a la reescritura del WebSocket en §22 antes de
hacerlo).

**Cambios**:
- `sdkconfig.defaults`/`sdkconfig`: `CONFIG_LWIP_MAX_SOCKETS=16` (antes
  10, para llevarlo a la par de `CONFIG_LWIP_MAX_ACTIVE_TCP=16` que ya
  estaba en 16 - hoy la tabla de descriptores de socket era el techo
  real, no los PCBs de TCP en sí), `CONFIG_LWIP_SO_LINGER=y`. Aplicado
  con edición directa de `sdkconfig` + `idf.py reconfigure` (no solo
  `sdkconfig.defaults` - lección de §20: `reconfigure` no pisa un valor
  ya resuelto).
- `TLSSocket::close()`: agrega `SO_LINGER{1,0}` antes de
  `mbedtls_net_free()`. No compite con el `mbedtls_ssl_close_notify()`
  agregado en §22 - uno es el aviso educado a nivel TLS, el otro es
  solo cómo se destruye la conexión TCP subyacente después. Afecta a
  TODO uso de `TLSSocket` (Login5, PUT de connect-state,
  ContextResolver, CDN, el WebSocket nuevo de §22), no solo al caso
  que lo disparó.
- `PlainConnection::close()` (AP/Mercury, compilado también para host/
  Win32, no solo ESP32): mismo `SO_LINGER{1,0}`, agregado antes del
  `shutdown(SHUT_RDWR)` ya existente (ese `shutdown()` cumple un rol
  distinto - desbloquea cualquier hilo que esté leyendo este socket en
  ese momento, no está ahí por motivos de cierre prolijo - se dejó sin
  tocar).

**Verificado**: `idf.py reconfigure` confirmó los valores aplicados
(`grep` directo sobre `sdkconfig` post-reconfigure). `idf.py build`
limpio, 50% de flash libre (sin cambio). Suite de host: 27/27 casos,
112/112 asserts (sin regresión - `PlainConnection.cpp` se compila
también para el build de host, así que este cambio sí corrió ahí,
a diferencia de §22 que es exclusivamente `#ifdef ESP_PLATFORM`).

**Pendiente confirmar en hardware**: que una sesión larga (el tipo de
uso donde apareció el `esp-tls: Failed to create socket` original) ya
no lo repita, o que si algo similar vuelve a pasar, sea con más margen
de sockets disponibles antes de llegar al límite.

**Revertido en parte (2026-07-15, mismo día) - `SO_LINGER` causó una
regresión real en hardware**: primera prueba tras flashear mostró un
loop de reconexión del Dealer completamente nuevo - `EspWebSocketTransport.cpp:
Dealer WebSocket: peer sent WebSocket close, disconnecting` repitiéndose
cada 10-20 segundos desde el arranque mismo, sin ninguna acción del
usuario de por medio (a diferencia del único evento de este tipo visto
en §22, que coincidía con un cambio de playlist real). Síntoma
acompañante: el mismo `transfer` (payload de 202 bytes, byte-a-byte
idéntico) se reprocesó 3 veces - el patrón exacto documentado en §10/
`DealerClient.cpp` como "el cliente reintenta porque nunca ve la
confirmación a tiempo" - consistente con que el WS nunca llegaba a
estabilizarse el tiempo suficiente para completar un ciclo de comando/
respuesta limpio.

**Hipótesis de la causa** (no confirmada con captura de paquetes,
razonada por correlación temporal y por descarte): un RST abortivo en
ALGUNA OTRA conexión (Login5, PUT de connect-state, CDN - todas pasan
por el mismo `TLSSocket::close()` recién modificado) podría estar
confundiendo al backend de Spotify. El Dealer WS y las llamadas a
`spclient` (PUT de connect-state) comparten el mismo
`Spotify-Connection-Id` - si el backend trata ambos como una sola
"sesión lógica de dispositivo", un cierre abrupto en cualquiera de las
dos mitades podría estar tirando abajo la otra como medida de
limpieza/seguridad del lado servidor.

**Acción tomada, mismo método que §20 (aislar la variable
sospechosa)**: revertido `SO_LINGER` únicamente - `CONFIG_LWIP_SO_LINGER`
vuelve a estar deshabilitado, y los dos `setsockopt(SO_LINGER)`
(`TLSSocket.cpp`, `PlainConnection.cpp`) fueron retirados, dejando solo
comentarios que documentan el intento y el motivo del revert (para no
reintentarlo a ciegas). `CONFIG_LWIP_MAX_SOCKETS=16` se mantiene - es
un cambio de presupuesto puro, sin implicancias de protocolo, no hay
motivo para sospechar que cause esto.

**Verificado tras el revert**: `idf.py reconfigure` confirmó
`LWIP_MAX_SOCKETS=16` + `LWIP_SO_LINGER is not set`. `idf.py build`
limpio, 50% de flash libre. Suite de host: 27/27 casos, 112/112
asserts (sin regresión).

**Pendiente confirmar en hardware**: que el revert efectivamente
elimina el loop de reconexión (probando exactamente el mismo escenario
- conectar el device y dejarlo un rato sin tocar nada). La sesión
anterior a esta (la que cerró §22) ya había validado
`EspWebSocketTransport.cpp` solo, sin `SO_LINGER`, con varios minutos
de reproducción estable y un único cierre limpio coincidiendo con un
cambio de playlist real - eso acota bastante la sospecha a lo agregado
en §23 y no al WebSocket propio en sí. Si aun así el loop persistiera
sin `SO_LINGER`, sería una segunda variable independiente por aislar,
pero la evidencia actual no apunta ahí.

---

## 24. Rotación de `session_id` por sesión de reproducción, no por vida del dispositivo (2026-07-15)

Disparado por un log del usuario con una observación afilada que refinó
todo el patrón de cierres del Dealer (§22): al hacer click en OTRA
playlist en el cliente de escritorio llegó primero un mensaje
`hm://playlist/v2/playlist/37i9dQZF1DX5trt9i14X7j` (196 bytes,
descartado por nuestro `DealerClient` como "unhandled"), ~18s después
el `request` `play` real, respondido `success=1`, y 33ms después el ya
conocido `peer sent WebSocket close`. Y el dato clave del usuario:
**pausar desde el cliente NUNCA cierra la conexión** - solo los
comandos que cambian de contexto.

**Hipótesis del usuario investigada primero**: ¿el cierre es porque
descartamos el mensaje de playlist? **No** - verificado contra
go-librespot (`daemon/player.go`): su handler de mensajes solo procesa
`hm://pusher/v1/connections/`, `connect/volume`, `connect/logout` y
`v1/cluster` - los mensajes `hm://playlist/v2/playlist/` **tampoco los
maneja**, los descarta igual que nosotros. Son notificaciones (tipo
`message`, no `request`) - no llevan respuesta. Descartado como causa.

**Pero la correlación play-con-contexto-nuevo → cierre / pause → nunca
llevó a revisar qué más difiere exactamente en ese camino, y ahí
apareció una divergencia real de protocolo**:

- **go-librespot `loadContext()`** (`daemon/controls.go`, el camino que
  ejecuta TODO comando `play` con contexto nuevo) **regenera
  `PlayerState.SessionId`** con 16 bytes aleatorios en base64, en cada
  carga de contexto:
  ```go
  sessionId := make([]byte, 16)
  _, _ = rand.Read(sessionId)
  p.state.player.SessionId = base64.StdEncoding.EncodeToString(sessionId)
  ```
- **go-librespot transfer** (`daemon/player.go`): **adopta** el id de
  la sesión transferida, o genera uno nuevo si no viene:
  ```go
  if sessId := transferState.CurrentSession.OriginalSessionId; sessId != nil {
      p.state.player.SessionId = *sessId
  } else { /* 16 bytes random -> base64 */ }
  ```
- **Nosotros (§16)**: `sessionId` generado UNA vez en el constructor y
  nunca cambiado - el mismo id para todos los contextos de toda la
  sesión del dispositivo. Peor: nuestro proto vendorizado ni siquiera
  decodificaba el campo (`Session` solo tenía `context = 2`;
  `original_session_id` es el campo **9**, verificado contra
  `protocol/proto/session.proto` de librespot-rust).

La semántica real de `session_id` es "esta sesión de REPRODUCCIÓN"
(un contexto cargado), no "este dispositivo desde que arrancó". El
backend crea una sesión nueva cuando el cliente pide reproducir un
contexto nuevo - y un dispositivo que responde `success` pero sigue
reportando el session_id de la sesión anterior en sus PUTs es
exactamente el tipo de inconsistencia que podría hacer que el backend
recicle la conexión del Dealer. **No confirmado como LA causa del
cierre** (sin captura del lado servidor sigue siendo inferencia), pero
es la única divergencia de protocolo encontrada que coincide
exactamente con el patrón observado: cambia-contexto → cierre, pause
(que no toca la sesión) → nunca.

**Cambios**:
- `protobuf/connectstate.proto`: `Session.original_session_id = 9`
  agregado (número de campo del proto oficial); `connectstate.options`:
  `FT_POINTER` como el resto de strings decode-only.
- `PlayerEngine`: nuevo helper privado
  `adoptOrRegenerateSessionId(const char*)` (adopta el id dado o genera
  16 bytes random → base64, bajo `engineMutex`).
  - `handleTransfer()`: lo llama incondicionalmente tras decodificar
    (antes de los `pb_release`), con
    `current_session.original_session_id` - las dos conductas de
    go-librespot en una.
  - `handlePlay()`: lo llama con `nullptr` (regeneración) justo antes
    de `loadTracks()` - mismo punto que `loadContext()` de go-librespot.
    El camino "already loaded, skipping reload" NO regenera (no se
    carga contexto nuevo).
- `sessionId` pasó de inmutable-tras-constructor a mutable: las dos
  lecturas (`putState()` y el PUT de `runTask()`) ahora lo leen bajo
  `engineMutex`, junto con `playbackId`/`contextUri` que ya estaban ahí.

**Sobre el gap de 18s del log** (click en playlist → request play):
no distinguible desde el dispositivo si fue demora de entrega del
servidor o navegación del usuario (click para abrir la playlist a los
:38 - que generó la notificación de playlist - y click en play a los
:55). El dispositivo respondió el request en ~1s desde que llegó,
incluyendo la resolución de contexto - no hay lentitud nuestra en ese
tramo.

**Verificado**: build de firmware limpio (proto regenerado por nanopb,
50% flash libre), suite de host 27/27 casos, 112/112 asserts.

**Pendiente confirmar en hardware**: mismo escenario exacto (reproducir
un rato, click en OTRA playlist desde el cliente) y ver si el cierre
desaparece. Si desaparece: la causa era la sesión estancada. Si
persiste: la rotación de session_id queda igual (es corrección de
protocolo respaldada por ambas referencias, correcta por sí misma), y
el cierre vuelve a la explicación de §22 (reciclado del backend,
tolerado por reconexión).

**Resultado en hardware (2026-07-15, mismo día)**: dos cambios de
playlist en la misma sesión con la rotación de session_id activa. El
primero (~2.5 min después de la última actividad del Dealer)
**sobrevivió sin cierre** - `play` + `update_context` procesados,
conexión intacta. El segundo (~5.5 min de idle a nivel mensajes)
**cerró igual**, mismo patrón exacto (`peer sent WebSocket close` 59ms
después de nuestra respuesta). Conclusión: la rotación de session_id
NO era la causa del cierre - queda mergeada igual por ser corrección
de protocolo respaldada por ambas referencias. Pero este par de logs
aportó el dato discriminante clave para §25: el factor no es el
comando en sí, es cuánto tiempo pasó sin MENSAJES de aplicación. Y el
detalle de los 59ms: menos que el RTT hasta guc3 - el servidor decidió
cerrar ANTES de poder leer nuestra respuesta; nada en nuestro camino de
respuesta (reply, PUT, session_id) puede influir en esa decisión.

---

## 25. Ping JSON de aplicación cada 30s, estilo go-librespot (2026-07-15)

La pieza que faltaba, encontrada al volver a go-librespot con la
pregunta correcta ("¿cómo mantiene la conexión NO-idle?") en vez de
"¿cómo maneja el cierre?": go-librespot manda, además de cualquier
mecanismo a nivel protocolo WS, un **mensaje de texto JSON**
`{"type":"ping"}` cada 30 segundos, incondicionalmente
(`dealer/dealer.go`):

```go
const pingInterval = 30 * time.Second
// ...
conn.Write(ctx, websocket.MessageText, []byte("{\"type\":\"ping\"}"))
```

y trackea el `{"type":"pong"}` JSON que el servidor responde
(`case "pong": d.lastPong = time.Now()`).

**Por qué esto lo cambia todo**: nuestros pings eran frames de control
WS (opcode 0x9) - mantienen vivo el camino TCP/NAT, pero
aparentemente NO cuentan como "actividad" para el timer del backend
que decide que una conexión Dealer está idle-y-reciclable: fluyeron
cada 30s (antes 15s) a través de TODOS los cierres observados sin
prevenir ninguno. go-librespot mantiene la conexión no-idle a nivel
MENSAJE - y no tiene ni una línea de código que trate estos cierres
como algo especial, consistente con que simplemente nunca los ve.
librespot-rust, que solo hace pings WS (como nosotros hasta ahora), los
trata como desconexión genérica - consistente con que sí los sufre y
reconecta en silencio. Y spotify_app (el otro proyecto del usuario)
nunca los ve porque nunca recibe comandos: el cierre perezoso solo se
manifiesta al entregar un request, y a un oyente pasivo no le llega
ninguno.

**La evidencia acumulada encaja completa**: cambios de contexto con
<2.5 min desde el último mensaje real sobrevivieron; con ~5.5 min o
más, cierre - el umbral del backend está en algún lugar entre esos dos
puntos, y con un ping de aplicación cada 30s la conexión nunca vuelve
a acercarse a él.

**Sobre la alternativa propuesta por el usuario** (reconexión proactiva
con connection-id nuevo al detectar cambio de contexto): descartada
por el dato de los 59ms de §24 - el servidor decide cerrar antes de
leer nuestra respuesta, así que reconectar después del comando no
previene nada; solo replicaría lo que el auto-reconnect ya hace en
~1.7s. El ping JSON ataca la causa (la ventana de idle previa al
comando), no el síntoma.

**Cambios** (`DealerClient.cpp` únicamente - es un mensaje de
aplicación, no pertenece al transporte):
- `JSON_PING_INTERVAL_MS = 30000`; en el loop de `runTask()`, cada 30s
  incondicionales (no solo en idle, igual que el `pingTicker` de
  go-librespot), `transport->sendText(R"({"type":"ping"})")` + log
  debug. Un fallo de envío ya voltea el transporte solo (fail() de
  §22-reauditoría) y el loop lo detecta en la siguiente vuelta.
- La rama `"pong"` de `handleMessage()` (que ya existía, tolerante)
  ahora loguea en debug - visibilidad para la verificación en hardware.
- Los pings WS de protocolo del transporte quedan como están: son la
  detección de peer muerto (30s idle → ping 0x9 → 7s sin respuesta →
  reconexión), un rol distinto. Con el ping JSON fluyendo, el
  transporte rara vez verá 30s de silencio puro - pero si la conexión
  muere de verdad, la cadena sigue intacta.

**Pendiente confirmar en hardware**: el escenario que hasta ahora
siempre cerró - reproducir >6 minutos sin tocar nada, luego click en
otra playlist desde el cliente. Con el ping JSON debería verse en el
log `Dealer: sent JSON ping` / `received JSON pong` cada 30s, y el
cambio de playlist debería procesarse SIN el `peer sent WebSocket
close`. Si aún así cierra, el timer del backend tampoco cuenta los
pings JSON propios como actividad (poco probable, dado que go-librespot
depende de exactamente esto) y ahí sí quedaría solo aceptar el ciclo
cerrar-reconectar como comportamiento del backend.

---

## 26. El ping JSON no era suficiente: el reply del Dealer salía ~2s antes que el PUT real - `putBufferingState()` síncrono antes de `loadTracks()` (2026-07-15)

Con el ping JSON de §25 ya en firmware, un log real de hardware el
mismo día siguió mostrando el cierre exacto de siempre: `play` ->
reply `success=1` -> `peer sent WebSocket close` **33ms después**.
El dato que descarta idle como causa de ESTE cierre puntual: el último
mensaje real del Dealer antes del `play` había llegado 18s antes (una
notificación de playlist, `:38.771` -> reply a las `:56.709`) - muy por
debajo tanto del intervalo de ping (30s, nunca llegó a dispararse en
esa ventana) como del umbral de ~2.5min que en §24 sobrevivió sin
cerrarse. El trigger es el comando `play` en sí, no inactividad - el
ping JSON ataca un problema distinto (idle a nivel mensaje) al que
efectivamente está pasando acá.

**Comparación de orden de operaciones contra go-librespot**, pedida
por el usuario (`daemon/controls.go`, `daemon/player.go`, `dealer/
dealer.go`, `dealer/recv.go`, en `/desarrollo/git/go-librespot`):

- **go-librespot**: `loadCurrentTrack()` (`daemon/controls.go:333-411`,
  llamada tanto desde `loadContext()` para `play` como desde el
  `transfer`) llama `p.updateState(ctx)` **síncronamente**, con
  `IsBuffering=true`/`IsPlaying=true`/`PlaybackSpeed=0`, **antes** de
  `p.player.NewStream(...)` (la parte cara: key exchange + resolución
  de CDN). `updateState()` (`daemon/player_state.go:164-175`) dispara
  un PUT real (coalescido a 200ms, no diferido más que eso). Recién
  cuando `loadCurrentTrack()` retorna - es decir, después del PUT de
  buffering, la carga del stream, y un segundo PUT con
  `IsPlaying=true`/`IsBuffering=false` - vuelve el control a
  `handleDealerRequest()` (`daemon/player.go:406-418`), y ahí
  `dealer/recv.go:247-251` recién manda el `reply` por el WebSocket.
  **El PUT sale siempre antes que el reply**, nunca al revés.

- **Nuestro C++ (antes de este cambio)**: `handlePlay()`/
  `handleTransfer()` resolvían el contexto y llamaban `loadTracks()`,
  que solo **encola** trabajo async (`trackQueue->updateTracks()`,
  corre en otra tarea) y retorna de inmediato - sin ningún `putState()`
  síncrono. `DealerClient.cpp` manda el `reply` (`success=1`) apenas
  `handlePlayerCommand()` retorna - milisegundos después de recibir el
  request. El PUT real (`reason 3`/`reason 4` del log, `NEW_DEVICE`/
  `PLAYER_STATE_CHANGED`) solo salía ~2s después, cuando el pipeline
  async de `TrackQueue` terminaba de resolver key+CDN - y en el log en
  cuestión ni siquiera llegó a salir antes del cierre: salió después de
  haber reconectado ya. **El reply salía ~2s antes que cualquier PUT.**

**Hipótesis**: el backend espera que un `play`/cambio-de-contexto venga
acompañado de un PUT de connect-state (al menos "buffering") en una
ventana corta después del ack - si el reply llega sin un PUT
correlacionado cerca, trata el estado del dispositivo como
desincronizado y recicla la conexión del Dealer. Explica el patrón
exacto observado en §24 y acá (cierre 33-59ms después del reply, antes
de que el servidor pudiera siquiera leer la respuesta) sin depender de
idle, y por qué el ping JSON (puramente keepalive, sin relación con el
contenido del estado) no lo tocó. No confirmado con captura del lado
servidor - sigue siendo la explicación que mejor encaja con toda la
evidencia acumulada en §22-§26.

**Cambios** (`PlayerEngine.h`/`.cpp`):
- Nuevo método privado `putBufferingState(trackUri, positionMs,
  paused)`: construye y manda un PUT síncrono (`PLAYER_STATE_CHANGED`,
  `is_active=true`, `is_playing=true`, `is_buffering=true`,
  `is_paused=paused`, `playback_speed=0` - mismo campo a 0 "no
  progresa mientras bufferea" que el comentario de go-librespot),
  con `session_id`/`context_uri` ya conocidos en ese punto y
  `track.uri` del track resuelto. Deliberadamente sin `playback_id`
  (go-librespot tampoco lo manda en esta primera llamada - recién se
  conoce cuando el stream abre de verdad).
- `handlePlay()` y `handleTransfer()`: ambos llaman
  `putBufferingState(...)` **antes** de `loadTracks(...)`, con el
  mismo `startIndex`/`positionMs`/`paused` que ya tenían resueltos -
  mismo punto relativo que `loadCurrentTrack()` en go-librespot. Como
  `handlePlayerCommand()` corre en la tarea propia de `DealerClient`
  (ya HTTPS-capable - `contextResolver.resolve()` ya hacía una llamada
  HTTP síncrona ahí mismo, y `putState()` para `NEW_DEVICE` ya se
  llama síncronamente desde esa misma tarea), agregar un PUT síncrono
  más no es un patrón nuevo, solo un PUT adicional en un punto donde ya
  se bloqueaba en red.
- Los casos de `transfer`/`play` que devuelven sin cargar tracks
  (`updatePlayerState(false, "", 0, 0)`, cola vacía) no se tocaron - no
  hay comando de reproducción real ahí, nada que anunciar como
  "buffering".

**Verificado**: `idf.py build` limpio (0 errores/warnings nuevos, 50%
flash libre, igual que antes). Suite de host (`unit_tests`) 27/27
casos, 112/112 asserts - sin cambios, porque `PlayerEngine.cpp`
está fuera del alcance de esa suite (no hay stub de spclient real para
las llamadas HTTPS de PUT; `CSPOT_SHARED_SOURCES` en `tests/
CMakeLists.txt` no lo incluye) - esto ya era así antes de este cambio,
no una regresión de cobertura introducida ahora.

**Pendiente confirmar en hardware**: el mismo escenario que cerró en
este log (click en playlist -> `play` con contexto nuevo). Con el
cambio, el log debería mostrar `connect-state PUT ok (reason 4)`
**antes** de `Dealer: request key=... endpoint=play -> success=1`, y
el `peer sent WebSocket close` no debería aparecer inmediatamente
después del reply. Si el cierre persiste igual, la hipótesis de este
párrafo queda descartada y el reciclado de conexión del backend habría
que aceptarlo sin más como comportamiento normal (tolerado por el
auto-reconnect de todas formas, ~1.7s).

**Resultado en hardware (2026-07-15, mismo día)**: confirmado que NO
lo arregló (§27 abajo) - orden correcto (PUT antes que el reply,
verificado en el log), cierre igual, ~50-107ms después. Pedaleado a
§27.

---

## 27. A/B contra go-librespot real: el cierre NO es un comportamiento universal del backend - falta un header `User-Agent` en nuestro handshake WS (2026-07-15)

Con dos hipótesis ya refutadas en hardware (§25 ping/pong, §26 orden
PUT/reply), la pregunta pasó de "¿cómo evitamos el cierre?" a "¿le pasa
esto también a un cliente de referencia real, en la misma red, contra
el mismo backend?" - si la respuesta es no, deja de ser "comportamiento
inherente del backend" y pasa a ser algo específico de nuestra
implementación.

**Setup**: se compiló y corrió `go-librespot` localmente (máquina del
usuario, misma red Wi-Fi que el ESP32, mismo `guc3-dealer.spotify.com`)
en modo Zeroconf, `log_level: debug`, salida de audio real por ALSA
(`audio_backend: pipe` + `aplay -t raw`, para poder escuchar que
efectivamente reproduce). Sin tocar el sistema: Go 1.25.5 y las libs
`libogg`/`libvorbis`/`libflac` (headers `-dev`) se instalaron en
userspace vía `apt-get download` + `dpkg-deb -x` a un prefix local
(sin `sudo`, no disponible en el entorno), y el binario de Go oficial
se bajó y extrajo directo del tarball - nada quedó instalado a nivel
sistema.

**Reproducido el escenario real**: el usuario seleccionó el device
Zeroconf desde su Spotify real, reprodujo, y mandó varios comandos
`play` con contexto nuevo (dos playlists distintas,
`spotify:playlist:623ebo27lRraU9p81mXDHR` y
`spotify:playlist:37i9dQZEVXbMMy2roB9myp`) más un `pause` - exactamente
la clase de comando que en nuestro C++ siempre gatilló el cierre.
**Ninguno cerró la conexión.** Verificado sobre el log completo
(`grep -i "dealer connection closed\|connected to.*dealer"`): un solo
`connected to guc3-dealer.spotify.com:443` inicial (22:56:20) y ningún
`dealer connection closed` posterior, a través de múltiples `play`,
`pause`, cambios de volumen y decenas de `put connect state` - la
conexión sobrevivió todo el resto de la sesión sin un solo corte.

(Nota al margen, no relacionada al hallazgo: la primera parte del test
tuvo dos problemas propios del setup, no de `go-librespot` - un
`cat audio.pipe > /dev/null` sin backpressure hizo que la reproducción
"saltara" todos los tracks en milisegundos, lo que aparentemente
disparó el sistema antiabuso de Spotify contra esa sesión puntual
-`refused the audio key (code 2)` en cadena -; y después `aplay` sin
`-t raw` moría al toque intentando leer un header WAV inexistente en
PCM crudo, dejando el FIFO sin lector. Ambos corregidos - `aplay -t raw`
como consumidor real - antes de las pruebas que importan, arriba.)

**La comparación que encontró la causa real**: con el cierre
descartado como "inevitable, todos lo sufren", tocaba comparar qué
hace distinto nuestra conexión WS de la de go-librespot. `dealer/
dealer.go:92-97` (`connect()`):

```go
websocket.Dial(ctx, fmt.Sprintf("wss://%s/?access_token=%s", addr, accessToken), &websocket.DialOptions{
    HTTPClient: d.client,
    HTTPHeader: http.Header{
        "User-Agent": []string{librespot.UserAgent()},
    },
})
```

`librespot.UserAgent()` (`version.go:56-58`) es solo
`"go-librespot/<version> Go/<go version>"` - ni siquiera intenta
parecer un cliente Spotify real, pero está presente.

`EspWebSocketTransport.cpp::handshake()` (`src/
EspWebSocketTransport.cpp:379-390`, y su gemelo de host
`PortableWebSocketTransport.cpp:140-151`) arma el request HTTP a mano,
línea por línea - `Host`, `Upgrade`, `Connection`,
`Sec-WebSocket-Key`, `Sec-WebSocket-Version`. **Sin `User-Agent`.**
Ninguno. El resto del proyecto sí lo manda siempre - `Login5Client.cpp`
lo pone en las dos llamadas HTTP que hace (`USER_AGENT = "cspot/1.0"`,
líneas 156/224) - este handshake WS armado a mano era la única
excepción real en toda la base de código.

**Hipótesis**: si el backend de Spotify (o algún proxy/edge delante de
`guc3-dealer.spotify.com`) aplica heurísticas de anomalía/anti-bot a
nivel de conexión, una conexión WS sin `User-Agent` es una señal barata
y obvia de "esto no es un cliente real" - independientemente del resto
del protocolo siendo perfecto. Encaja con que sea específicamente el
Dealer (WS persistente, visible como conexión de larga duración) y no
las llamadas HTTP normales (que sí llevan el header) las que sufren
esto.

**Cambios**: agregado `User-Agent: cspot/1.0\r\n` al request de
handshake en ambos transportes (`EspWebSocketTransport.cpp` -el que
compila para ESP32- y `PortableWebSocketTransport.cpp` -el de host,
para que no diverja del real).

**Verificado**: `idf.py build` limpio (0 errores/warnings nuevos, 50%
flash libre). `ws_transport_echo_test` (contra `ws_echo_server.py`
local) - los 9 checks pasan igual, el header extra no rompe el
handshake RFC 6455 esperado por un servidor WS estricto.

**Pendiente confirmar en hardware**: el mismo escenario de siempre
(reproducir, cambiar de playlist con un `play` de contexto nuevo). Si
el `User-Agent` era la causa, el `peer sent WebSocket close` debería
dejar de aparecer. Si persiste, la comparación A/B contra go-librespot
sigue siendo válida (algo más distingue nuestra conexión) pero este
header en particular no era la variable - habría que capturar el
handshake completo de ambos clientes (tcpdump/mitmproxy) para ver qué
más difiere, ya que a nivel de código fuente esta era la única
diferencia real encontrada en el handshake.

**Resultado en hardware (2026-07-15, mismo día)**: confirmado que el
`User-Agent` tampoco lo arregló - mismo patrón de cierre exacto con el
header puesto. Con dos hipótesis de protocolo/aplicación ya
descartadas en hardware real (§25, §26) y ahora también el único header
de handshake que diferenciaba a go-librespot (§27), el pedido del
usuario fue explícito: preservar la implementación RFC 6455/mbedTLS
propia sin tocarla más, y probar directo con el cliente oficial de
ESP-IDF para aislar si el problema vive más abajo de lo que el código
fuente puede mostrar (fingerprint TLS, orden de headers a nivel de
socket, framing) - la siguiente sección.

---

## 28. Vuelta a `esp_websocket_client` (oficial) como A/B contra el cliente RFC 6455 propio (2026-07-15)

Antes de tocar nada, se hizo commit de todo el trabajo pendiente en la
rama (`git commit -m "."`, siguiendo el estilo de checkpoints ya
establecido acá) - la implementación RFC 6455/mbedTLS propia
(`EspWebSocketTransport.cpp`, `PortableWebSocketTransport.cpp`,
`TLSSocket.{h,cpp}`) queda íntegra en el historial de git en ese commit,
recuperable con un simple `git revert`/`checkout` si este experimento no
resuelve nada.

**Qué se restauró**: la versión de `EspWebSocketTransport.cpp` previa a
§22 (la que envolvía `esp_websocket_client`, reemplazada en su momento
por bugs reales de esa librería - reconexión silenciosa sin poder
distinguir timeout de conexión muerta, presión sobre el presupuesto de
sockets del ESP32) se recuperó tal cual del historial de git (commit
`f343603`) y se guardó como archivo **nuevo**,
`EspWebSocketClientTransport.cpp` - no se sobreescribió el archivo
existente, para que ambas implementaciones convivan en el árbol.

**Cómo quedó cableado**: `EspWebSocketTransport.cpp` (la implementación
propia) se deshabilitó cambiando su guarda de `#ifdef ESP_PLATFORM` a
`#if defined(ESP_PLATFORM) && 0` - el archivo entero sigue ahí sin
tocar, simplemente no se compila; volver a activarla es esa única
línea. `EspWebSocketClientTransport.cpp` es el que ahora provee
`WebSocketTransport::create()` para ESP32 (sin el `&& 0`, así que sin
colisión de símbolos al linkear). Se le agregó el mismo header
`User-Agent: cspot/1.0` que §27 (vía `esp_websocket_client_config_t.
headers`, campo de líneas CRLF crudas) - así este A/B aísla una sola
variable (la implementación de transporte) en vez de reintroducir
"sin User-Agent" como una segunda diferencia junto con "cliente
distinto".

**Cambios de build**: `idf_component.yml` recupera
`espressif/esp_websocket_client: "^1.2"` (mismo pin que tenía antes de
§22, recuperado del historial de git); `CMakeLists.txt` recupera
`espressif__esp_websocket_client` en `PRIV_REQUIRES` y el
`target_link_libraries(cspot PRIVATE idf::espressif__esp_websocket_client)`
correspondiente, también restaurados de antes de §22.

**Verificado**: `idf.py build` limpio (0 errores, `dependencies.lock`
resolvió e incorporó el componente automáticamente, 50% flash libre -
el binario creció un poco por la librería nueva pero el margen no
cambió de forma perceptible). Suite de host (`unit_tests`) 27/27,
112/112 asserts - sin cambios, ninguno de los archivos tocados en este
punto es parte de esa suite (todo detrás de `#ifdef ESP_PLATFORM`).

**Pendiente confirmar en hardware**: el mismo escenario de siempre. Si
el cierre desaparece con `esp_websocket_client`, la causa vive en algo
específico del cliente RFC 6455 propio (fingerprint TLS/framing/orden
de bytes en el handshake) - a investigar puntualmente qué, sin
necesariamente quedarse con el cliente oficial de forma permanente
(reabre los bugs de §22). Si el cierre persiste igual incluso con el
cliente oficial, la causa queda descartada como algo de nuestro código
en cualquiera de sus dos formas y pasa a ser explícitamente algo del
lado servidor no relacionado al transporte WS en sí (headers de
autenticación, timing de la sesión Login5/AP, u otra cosa fuera de la
conexión Dealer misma) - haría falta revisar variables más allá del
WebSocket.

**Resultado en hardware (2026-07-15, mismo día)**: concluyente, y en
dos partes.

1. **El cierre persistió, idéntico**, con `esp_websocket_client`:
   `request ... endpoint=play -> success=1` seguido de
   `DealerClient.cpp:440: failed sending reply` (el socket ya estaba
   muerto del lado del cliente oficial cuando se intentó mandar la
   respuesta) y reconexión - mismo patrón exacto que con el cliente
   propio, solo que el mensaje de error cambia de forma porque
   `esp_websocket_client` reporta el socket muerto en el send en vez de
   en el receive. **Esto descarta definitivamente al cliente RFC 6455
   propio como causa** - dos implementaciones de WebSocket
   completamente distintas, mismo comportamiento exacto ante el mismo
   comando.
2. **Bonus no buscado**: en el mismo log, ~30s antes del cierre de
   siempre y sin ningún comando del Dealer en vuelo (plena reproducción
   idle), apareció textual el bug original que motivó escribir el
   cliente propio en primer lugar (§22): `esp_transport_ws_poll_
   connection_closed: unexpected data readable on socket` /
   `Connection terminated while waiting for clean TCP close`. No fue
   una casualidad de aquella sesión de hardware de §22 - es un
   comportamiento reproducible de la librería oficial, y confirma que
   la decisión de reemplazarla seguía siendo correcta independientemente
   de este experimento.

**Conclusión y acción**: con el cliente oficial sumando su propio bug
sin resolver el que veníamos persiguiendo, se revirtió a
`EspWebSocketTransport.cpp` (el cliente propio) como base - estrictamente
mejor opción entre las dos (mismo cierre sin resolver, cero bugs
extra). Cambio de una línea en cada archivo (la guarda `#ifdef
ESP_PLATFORM` vuelve a activar el propio, `#if defined(ESP_PLATFORM)
&& 0` desactiva - no borra - `EspWebSocketClientTransport.cpp`, que
queda en el árbol por si algún hallazgo futuro apunta de nuevo
específicamente a la capa WS). `idf_component.yml`/`CMakeLists.txt`
quedaron con la dependencia a `espressif/esp_websocket_client` sin
sacar (no molesta, el archivo que la usaba está deshabilitado) para no
tener que re-cablearla si se retoma el experimento.

**Verificado**: `idf.py build` limpio, mismo tamaño de binario que
antes de §28 (0x1fbe40, 50% flash libre - confirma que
`EspWebSocketClientTransport.cpp` deshabilitado no aporta peso).
Suite de host 27/27, 112/112 asserts.

**Dónde queda la investigación**: con ping/pong (§25), orden PUT/reply
(§26), header `User-Agent` (§27) y la librería WS entera (§28) las
cuatro descartadas en hardware real, y la comparación A/B contra
go-librespot (§27) mostrando que un cliente en Go, misma red, mismo
backend, nunca sufre esto - lo único que sigue en pie como
diferenciador real entre "funciona" y "no funciona" es algo por debajo
de la capa WS que las dos implementaciones ESP32 comparten (ambas
corren sobre mbedTLS vía `esp-tls`/`bell::TLSSocket`) y que el stack
TLS de Go no comparte: el fingerprint del `ClientHello` (cipher suites
ofrecidas, extensiones, orden), o directamente algo del lado servidor
ajeno al transporte (cuenta, sesión, device_id) que ninguna captura de
tráfico del lado cliente podría revelar sin más. Confirmarlo
requeriría una captura de paquetes real (tcpdump/mitmproxy) comparando
el `ClientHello` de ambos clientes byte a byte - una inversión bastante
mayor a las anteriores, con un payoff incierto dado que el
comportamiento ya se tolera bien (reconexión automática en ~1.7s, sin
afectar el audio en curso). Queda como decisión del usuario si vale la
pena seguir, o si conviene pasar a las opciones de "contención" ya
discutidas (achicar la ventana de reconexión cacheando el host del
dealer, bajar el nivel de log del cierre de error a info) en vez de
seguir buscando la causa raíz.

**Nota aparte sobre el warning de `esp_websocket_client` (mismo día)**:
el usuario aportó una explicación externa (contexto LiveKit/WebRTC) de
que `unexpected data readable on socket` suele ser síntoma de OTRA
conexión fallando primero y arrastrando al WebSocket de señalización en
la carrera de cleanup, no la causa raíz en sí. Se verificó contra el
código fuente real (`esp-idf/components/tcp_transport/transport_ws.c:
1145-1180`, `esp_transport_ws_poll_connection_closed()`): en nuestro
caso esa lectura no aplica igual - la función **solo** se llama desde
dentro del manejo de un frame WS `CLOSE` (opcode 0x08) ya recibido y ya
respondido (línea 1122), como poll final para confirmar el cierre TCP
limpio; el warning aparece si en ese punto todavía hay datos legibles
en el socket en vez del FIN esperado. Para que este log exista, el
Dealer **ya mandó un frame `CLOSE` real** sobre esa misma conexión - no
es una carrera con un subsistema no relacionado arrastrando al WS
(como en el escenario de LiveKit), es el propio servidor cerrando el
Dealer, detectado por dos caminos de código distintos en las dos
implementaciones probadas. Cierra la posibilidad de que sea un
artefacto/race del lado ESP32 en vez de abrir una pista nueva - refuerza
la lectura de "cierre deliberado del servidor" ya sostenida desde §22.

---

## 29. Reauditoría de código a pedido del usuario, sin nuevo hallazgo de causa raíz (2026-07-16)

Antes de invertir en captura de paquetes (la única vía que quedaba según
§28), el usuario pidió releer el código una vez más buscando un bug de
implementación todavía no visto - en vez de asumir que ya no queda nada
del lado cliente. Repasados con detalle `EspWebSocketTransport.cpp`,
`TLSSocket.{h,cpp}`, `DealerClient.cpp` y el camino
`handleMessage()`→`handlePlayerCommand()`→`handleTransfer()`/`handlePlay()`
de `PlayerEngine.cpp`.

**Ningún bug nuevo explica el cierre en sí**, pero la relectura sí
confirma por qué el patrón de log ("success=1" seguido a milisegundos por
"peer sent WebSocket close") hace parecer que el comando dispara el
cierre sin que eso sea necesariamente cierto: `DealerClient::handleMessage()`
llama a `handlePlayerCommand()` de forma síncrona, en el mismo hilo que lee
el socket del Dealer. Para `transfer`/`play` eso implica `contextResolver.
resolve()` (HTTP/Mercury) y `putBufferingState()` (PUT HTTP a spclient)
antes de que `sendReply()` se llame - mientras ese hilo está ocupado en
esas llamadas de red, nadie llama a `receiveMessage()`, así que un close
que el servidor ya mandó simplemente espera en el buffer TCP hasta que el
hilo vuelve a leer. El comando no causa el cierre; solo tapa el momento
exacto en que llegó. Coincide con un comentario ya existente en
`PlayerEngine.cpp` (línea de `handlePlay()`, cerca de
`adoptOrRegenerateSessionId`): los cierres observados correlacionan
precisamente con comandos que cambian de contexto, nunca con `pause` -
mismo patrón que §24 ya documentó.

**Dos hallazgos menores, ninguno es la causa raíz**:
1. `TLSSocket::read()` no distinguía `MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY`
   (cierre TLS limpio del peer) de cualquier otro error - ambos caían en
   el mismo mensaje genérico `"read error..."`. No cambiaba el
   comportamiento (los dos casos llaman `fail()` y reconectan igual), pero
   si alguna vez el cierre llega como close-notify TLS antes que como
   frame WS 0x8, hoy quedaría invisible en el log cuál de los dos fue.
2. `DealerClient::transport` (un `unique_ptr` sin mutex) se reasigna en
   `connectOnce()` (hilo de `runTask()`) y se desreferencia en `stop()`,
   que puede llamarse desde otro hilo - race de datos real, pero solo
   importa en el apagado del dispositivo, no en el cierre recurrente en
   medio de sesión que se viene investigando. No corregido en este pase
   (fuera de alcance de lo pedido); queda anotado para revisarlo si algún
   día se toca el ciclo de vida de `DealerClient`.

**Implementado (hallazgo 1 solamente, a pedido del usuario)**: nuevo
`TLSSocket::peerClosedCleanly` (paralelo a `timedOut`), expuesto como
`lastReadWasPeerCloseNotify()`. `EspWebSocketTransport.cpp` gana un
`describeReadFailure(genericReason)` privado que todos los sitios de
`fail(...)` por error de lectura (no timeout) consultan, para loguear
`"peer closed TLS connection (close_notify)"` en vez del mensaje genérico
cuando aplica. Puramente de diagnóstico - no cambia ningún comportamiento,
solo qué dice el log la próxima vez que se observe un cierre en hardware.

**Verificado**: build de host limpio, suite `unit_tests` 27/27, 112/112
asserts sin regresión (`TLSSocketStub.cpp` no necesitó tocarse -
`peerClosedCleanly`/`lastReadWasPeerCloseNotify()` son privados/inline en
el header). `ws_transport_echo_test` no se pudo correr en este entorno
(falla con `Could not connect to 127.0.0.1` incluso en el código sin
tocar de antes de este cambio - limitación de red del sandbox, no
regresión introducida acá). Pendiente confirmar en hardware real: no se
espera ningún cambio de comportamiento, solo un mensaje de log distinto
si alguna vez aparece un close-notify TLS puro en vez de (o antes que) el
frame WS 0x8 ya observado hasta ahora.

**Conclusión**: con la reauditoría de código sin encontrar una causa raíz
nueva, las opciones que quedan siguen siendo las mismas de §28 - captura
de paquetes (payoff incierto) o pasar a mitigación (cachear host del
dealer, bajar el log de error a info) - queda como decisión del usuario.

**Limpieza de build, a pedido del usuario (mismo día)**: con
`EspWebSocketTransport.cpp` confirmado como la implementación que se sigue
usando (no un toggle temporal para el A/B de §28), se sacó la dependencia
a `espressif/esp_websocket_client` de `idf_component.yml` y de
`components/cspot/CMakeLists.txt` (`PRIV_REQUIRES` y el
`target_link_libraries` correspondiente) - ya no aporta nada al binario
real, solo peso muerto, dado que `EspWebSocketClientTransport.cpp` (el
único archivo que la usaba) sigue `#if 0`'d desde §28. El archivo en sí
**no se borró** - queda en el árbol tal como §28 decidió, solo que
reactivarlo para un futuro A/B ahora también requiere restaurar esas dos
entradas de build, no solo su propio guard. El comentario de
`CMakeLists.txt` (que describía a `EspWebSocketTransport.cpp` como el
deshabilitado temporalmente, al revés de la realidad desde el cierre de
§28) se corrigió de paso. `dependencies.lock` se editó a mano para sacar
la entrada de `esp_websocket_client` (sin recalcular `manifest_hash` - no
hay herramienta `idf.py` en este entorno para hacerlo; el próximo
`idf.py build` real lo va a detectar como desincronizado y
re-resolverlo solo, mismo comportamiento ya observado en §28 cuando el
lock incorporó el componente automáticamente). **Pendiente**: confirmar
`idf.py build` limpio en un entorno con ESP-IDF real - no verificable acá.
Build de host y suite `unit_tests` sin cambios (ninguno de estos archivos
toca esa rama).

---

## 30. "Spotify can't play this right now" al cambiar de playlist - causa raíz real, distinta del cierre del WS (2026-07-16)

El usuario reportó, con log real, que al cambiar de playlist desde el
cliente aparece el error del título en la app - y trajo el log exacto de
una de estas veces. A diferencia de §22-§29 (el cierre del Dealer WS en
sí, ya aceptado como comportamiento del backend), esta vez el log mostró
algo nuevo: la sesión Mercury/AP (`Session.cpp`/`MercurySession.cpp`,
conexión separada del Dealer WS) **muere al mismo tiempo** que el Dealer
(`MercurySession.cpp:76: Error while receiving packet: Error in read`,
~130ms después de `EspWebSocketTransport.cpp: peer sent WebSocket close`)
- justo cuando un pedido de audio key (`MercurySession: Executing Mercury
Request, type GET`) estaba en vuelo para el track del `play` que acababa
de llegar por el Dealer.

**Cadena de causa encontrada, verificada leyendo el código (no solo
hipótesis)**:

1. **`MercurySession::failAllPending()` (`MercurySession.cpp:241`) sólo
   fallaba el mapa `callbacks`, nunca `audioKeyCallbacks`** - el mapa que
   `requestAudioKey()` (`TrackQueue.cpp:299`, pedido de la key del audio)
   usa. Cuando `recvPacket()` revienta (la caída de arriba) y se llama
   `failAllPending()` antes de `reconnect()`, un pedido de key en vuelo
   nunca recibe su callback - ni éxito ni fallo. `QueuedTrack::state`
   queda en `PENDING_KEY` para siempre.
2. `TrackPlayer.cpp:169` espera ese resultado con
   `track->loadedSemaphore->twait(5000)` - con el bug de (1), esa espera
   **siempre agota los 5 segundos completos** en vez de fallar apenas la
   conexión se sabe muerta. Recién ahí loguea `"Track failed to load,
   skipping it"` y salta el track (`eofCallback()`).
3. **Ese `eofCallback()` (mismo camino para EOF real, track fallido, o
   fallo al abrir el stream CDN - `TrackPlayer.cpp:182/209/368`) sólo
   dispara `PlayerEngine::notifyAudioEnded()` si la cola quedó
   vacía (`trackQueue->isFinished()`) - y `notifyAudioEnded()` no mandaba
   ningún PUT de connect-state**, sólo bookkeeping local
   (`isPlayingState=false`, `trackPlayer->resetState(true)`).

Resultado: `putBufferingState()` (§26) ya había anunciado
`is_buffering=true, is_playing=true` al empezar a procesar el `play` -
correcto hasta ahí - pero si el track después nunca llega a cargar (por
la caída simultánea de Mercury/AP, o cualquier otro motivo), **nada
corrige ese estado nunca**. El cliente de Spotify queda esperando una
actualización de `PlayerState` que confirme que arrancó (o que falló) y
que nunca llega - tras su propio timeout, muestra "Spotify can't play
this right now". El comentario de `handlePlay()` (línea cerca de
`adoptOrRegenerateSessionId`, ya citado en §29) sobre que los cierres
correlacionan con comandos que cambian de contexto es la otra mitad de
esta misma historia: cambiar de contexto es precisamente lo que dispara
el fetch de key que puede quedar atrapado en la ventana de reconexión.

**Hallazgo adicional, related pero independiente**: `updatePlayerState()`
(runTask() de `PlayerEngine`) fijaba `ps.is_buffering =
ps.is_paused` - conceptualmente equivocado (buffering es "el audio
todavía no cargó", no "está en pausa"). Como esta función sólo corre tras
un evento `TRACK_INFO`/`PLAY_PAUSE` genuino (es decir, con el track ya
cargado y produciendo frames), la carga ya terminó en ambos casos -
pausado o no. Con el bug, pausar un track ya cargado lo reportaba como
"todavía cargando" en vez de simplemente "pausado".

**Implementado, los tres fixes acordados con el usuario**:

1. `MercurySession::failAllPending()` ahora también saca (`std::move`) y
   falla `audioKeyCallbacks` bajo el mismo `sessionMutex`, invocando cada
   callback con `(false, {})` - mismo patrón ya usado para `callbacks`.
   Un pedido de key en vuelo durante una caída de conexión ahora falla
   inmediato en vez de esperar 5s completos.
2. `PlayerEngine::notifyAudioEnded()` ahora llama
   `updatePlayerState(false, "", 0, 0)` al final - mismo helper ya usado
   en las ramas "nada que cargar" de `handleTransfer()`/`handlePlay()` -
   así la cola agotada (sea por EOF real o por haberse rendido tras
   fallar) siempre corrige el estado que el cliente ve, en vez de dejarlo
   colgado en lo último que `putBufferingState()` anunció.
3. `updatePlayerState()`: `ps.is_buffering` pasa a ser incondicionalmente
   `false` en vez de `ps.is_paused`.

**Verificado**: build de host limpio. `unit_tests` 27/27, 112/112
asserts. `f93_concurrency_test` (TSan, ejercita exactamente
`execute()`/`requestAudioKey()` concurrente con `failAllPending()`) sin
detectar race - el test más relevante para el cambio de (1), limpio.
`f87_logger_concurrency_test` sin cambios. `PlayerEngine.cpp` no
es parte de la suite de host (no está en `CSPOT_SHARED_SOURCES`), así que
(2)/(3) sólo se verificaron por lectura, no por test automatizado.
**Pendiente confirmar en hardware real**: que un `play`/`transfer` cuyo
track se ve interrumpido por la misma caída de conexión ya no deje al
cliente colgado - debería, en el peor caso, mostrar el estado real
(detenido) en vez de "can't play this right now", y el track debería
fallar en ~0s en vez de 5s si la conexión ya se sabe muerta en ese
momento.

---

## 31. Análisis de código de `go-librespot` (clon local en `/desarrollo/git/go-librespot`) - por qué su Dealer WS no se cierra, dos diferencias reales nunca comparadas antes (2026-07-16)

El usuario pidió un análisis a fondo de por qué la referencia Go nunca
sufre este cierre, releyendo su código fuente real (no de memoria).
Revisados `dealer/dealer.go`, `dealer/recv.go` y la librería WS que usa
(`github.com/coder/websocket v1.8.14`, presente en el cache local de
módulos Go, `/home/user/go/pkg/mod/github.com/coder/websocket@v1.8.14`).

**Hallazgo principal - el ping corre en una goroutine separada,
independiente del handler del comando**: `Dealer.startReceiving()`
(`dealer.go:122-129`) lanza `go d.pingTicker()` y `go d.recvLoop()` como
dos goroutines separadas. `recvLoop()` → `handleRequest()`
(`recv.go:236-256`) sí bloquea esperando la respuesta real del handler
del comando (`<-resp`), exactamente igual que `cspot` - eso ya se sabía
desde §22. La diferencia real está en qué pasa **mientras** ese bloqueo
ocurre: `pingTicker()`, en su propia goroutine, sigue mandando el ping
JSON cada 30s sin importar que `recvLoop` esté ocupado, porque
`coder/websocket`'s `*Conn` lo permite explícitamente - `conn.go:30`:
`"All methods may be called concurrently except for Reader and Read."`
Esto se apoya, en última instancia, en que `crypto/tls.Conn` de Go sí
documenta ser seguro para lectura/escritura concurrente desde goroutines
distintas.

**`cspot` no puede replicar esto tal cual**: `EspWebSocketTransport`/
`DealerClient` corren deliberadamente en un solo hilo (§22) precisamente
porque mbedTLS *no* documenta esa misma garantía para
`mbedtls_ssl_context` - concurrencia de lectura/escritura sin verificar
fue explícitamente evitada, no un descuido. Consecuencia real, confirmada
releyendo el código: mientras `DealerClient::handleMessage()` está
bloqueado dentro de `contextResolver.resolve()`/`putBufferingState()`
(HTTP síncrono, ~1-2s según los logs reales ya vistos en esta
conversación), **ni el ping WS (`EspWebSocketTransport`) ni el ping JSON
(`DealerClient`) se mandan, ni se lee nada** - el Dealer queda mudo desde
el lado cliente durante exactamente esa ventana. `go-librespot` nunca
tiene ese apagón.

Esto encaja mejor que cualquier hipótesis previa con el patrón ya
confirmado empíricamente (§22: "comando después de idle → cierre; comando
inmediato → sin cierre") - es precisamente durante el procesamiento de un
comando que cambia de contexto cuando `cspot` dejar de emitir tráfico, y
`go-librespot` nunca lo hace.

**Hallazgo secundario - TCP keepalive a nivel de socket**:
`session.go:65`, `s.client = &http.Client{Timeout: 30 * time.Second}` -
sin `Transport` explícito, usa `http.DefaultTransport`, cuyo dialer fija
`KeepAlive: 30 * time.Second` (activa `SO_KEEPALIVE` a nivel de socket
TCP). `dealer.NewDealer()` (`dealer.go:53-58`) reutiliza ese mismo
`Transport`, así que el socket del Dealer lo hereda. `TLSSocket::open()`
(`TLSSocket.cpp`) nunca llama `setsockopt(SO_KEEPALIVE, ...)` - solo
`TCP_NODELAY` y `SO_SNDTIMEO`. Opera por debajo de TLS/WS, invisible a la
aplicación, pero es una señal más que infraestructura de red
intermedia (LBs, NAT) puede usar para decidir si una conexión sigue
"viva" del lado cliente.

**Descartado de nuevo, con evidencia directa de esta relectura** (no
solo repetido de la investigación anterior):
- Compresión/extensiones WS: `coder/websocket`'s `CompressionMode`
  default es `CompressionDisabled` (confirmado en su propio
  `options.go`) - ninguna de las dos implementaciones ofrece
  `permessage-deflate`. No es un diferenciador.
- Header `User-Agent`: ya estaba en el dial real de `go-librespot`
  (`dealer.go:95`) desde siempre - coincide con que agregarlo a `cspot`
  (§27) no cambió nada.

**Sigue sin poder confirmarse sin captura de paquetes**: fingerprint del
`ClientHello` (mbedTLS vs Go `crypto/tls` - cipher suites, y ALPN: Go
intenta ofrecer `h2` por defecto vía `ForceAttemptHTTP2` en su
`http.DefaultTransport`, mbedTLS acá no ofrece ALPN en absoluto). Sigue
siendo el otro candidato de §28, no evaluable desde el código fuente
solo.

**No implementado todavía** - análisis puramente de lectura de código,
sin cambios. Dos acciones posibles quedan sobre la mesa, ninguna
aplicada: (a) agregar `SO_KEEPALIVE` a `TLSSocket::open()` (cambio
barato, bajo riesgo, valor incierto sin confirmar que el servidor
efectivamente responda a esa señal), (b) investigar si
`mbedtls_ssl_context` realmente tolera lectura/escritura concurrente
desde dos hilos en la práctica (más allá de que no esté documentado
oficialmente) para replicar el diseño de goroutina separada de
`go-librespot` - cambio de arquitectura más profundo en
`EspWebSocketTransport`/`DealerClient`, revisitando una decisión que §22
tomó deliberadamente por prudencia, no por descarte de la idea.

---

## 32. `update_context` nunca mandaba un PUT propio - hipótesis nueva y concreta, corregida (2026-07-16)

El usuario aportó dos piezas de evidencia nuevas: (1) confirmación de que
el comando específico que precede el cierre en el log real es
`update_context` (`hm://playlist/v2/playlist/...` llega antes, sin
manejador - "unhandled in 5.2" - y es lo que dispara el `update_context`
que sigue), y (2) el código real de `go-librespot` para ese caso
(`daemon/player.go`) y su mecanismo de PUT coalescido
(`player_state.go`).

```go
case "update_context":
    if req.Command.Context.Uri != p.state.player.ContextUri {
        p.app.log.Warnf("ignoring context update for wrong uri: %s", ...)
        return nil
    }
    p.state.player.ContextRestrictions = req.Command.Context.Restrictions
    // ...ContextMetadata...
    p.updateState(ctx)
    return nil
```

**Lo que esto revela**: `go-librespot` **siempre** llama a
`updateState()` (su PUT coalescido, `statePutMinInterval=200ms` - mismo
valor que `PUT_MIN_INTERVAL_MS` en este proyecto) cuando el
`update_context` es real (uri coincide con el contexto activo) - resend
del `PlayerState` ya cacheado (`Track`/`Duration`/etc., sin recalcular
nada), solo con la metadata/restrictions nuevas aplicadas. El
`update_context` de `cspot` (`PlayerEngine::handlePlayerCommand()`)
**nunca mandaba ningún PUT** - solo hacía `return true` incondicional,
sin siquiera comparar el uri entrante contra `contextUri`. Coincide,
además, con que el comando específico que precede el cierre en el log
real es justo éste - el único paso de la secuencia
`play`→`update_context`→cierre donde `cspot` no hacía absolutamente nada
del lado servidor.

**Hipótesis**: si el backend espera *algún* PUT de reconocimiento tras un
`update_context` real (coincida o no con lo que hace `go-librespot`
exactamente), y nunca lo recibe, podría tratar el connect-state de este
dispositivo como desincronizado/no respondiendo y forzar un ciclo de
reconexión del Dealer como consecuencia - un mecanismo más específico y
más barato de probar que el candidato de §31 (que requeriría revisar la
concurrencia de `mbedtls_ssl_context`).

**Implementado**: `PlayerEngine::handlePlayerCommand()`'s caso
`update_context` ahora replica la lógica real de `go-librespot`:
- Extrae `command.context.uri`, lo compara contra el `contextUri`
  interno (ya trackeado, usado por `handleTransfer()`/`handlePlay()`).
- Si no coincide (o viene vacío): loguea y ack `true` sin mandar nada -
  mismo comportamiento que antes, ahora explícito en vez de incondicional.
- Si coincide: llama a `updatePlayerState(isPlayingState,
  lastKnownTrackUri, getPositionMs(), lastKnownDurationMs)` - **nuevo**
  par de miembros `lastKnownTrackUri`/`lastKnownDurationMs`
  (`PlayerEngine.h`), cacheados en `runTask()` cada vez que un PUT
  real se manda, exactamente el mismo patrón que
  `go-librespot`: resend del estado ya conocido, sin recalcular ni
  resolver nada nuevo - `cspot` sigue sin almacenar
  metadata/restrictions (schema recortado, `connectstate.proto`), pero
  ahora al menos emite un PUT de reconocimiento cuando el `update_context`
  es real.

**Verificado**: build de host limpio, `unit_tests` 27/27, 112/112 asserts
sin regresión. `PlayerEngine.cpp`/`.h` no son parte de la suite de
host (no están en `CSPOT_SHARED_SOURCES`) - el cambio se verificó por
lectura cuidadosa (mismo patrón ya usado en `handlePlay()` para
extraer `context.uri` de un `cJSON*`), no por test automatizado ni build
real de ESP-IDF (no disponible en este entorno). **Pendiente confirmar en
hardware real**: que el cierre del Dealer WS ya no aparezca (o aparezca
con menor frecuencia) en la secuencia `play`→`update_context` con esta
PUT de reconocimiento en el medio - si el cierre persiste igual, esta
hipótesis también queda descartada y refuerza aún más la lectura de
"comportamiento del backend, no corregible desde el cliente" ya sostenida
desde §22.

**Primera prueba en hardware (2026-07-16, mismo día) - no concluyente
para esta hipótesis puntual, pero confirma F30**: log real traído por el
usuario. Esta vez el cierre pasó justo después de un `play` **solo** -
sin ningún `update_context` en la secuencia (`endpoint=play ->
success=1` a las `16:27.509`, cierre 34ms después, sin ningún
`endpoint=update_context` entre medio). Como el camino que este fix
corrige nunca se ejerció en esta corrida puntual, **no confirma ni
refuta la hipótesis de este parágrafo** - hace falta una corrida donde el
cierre venga justo después de un `update_context` real (como en los logs
que motivaron este fix) para evaluarla. Confirma en cambio, otra vez, el
patrón más amplio de §22: cualquier comando que cambia de contexto
después de idle dispara el cierre, no es exclusivo de `update_context`.

Dato bueno aparte: a diferencia de la corrida que motivó F30 (§30), esta
vez la recuperación fue limpia - Mercury/AP no murió junto con el Dealer,
el fetch de metadata/audio key/CDN del track siguió sin interrupción
inmediatamente después de la reconexión, y el usuario confirmó que la
app reprodujo sin mostrar "can't play this right now". Consistente con
que F30 es una mitigación para cuando Mercury sí muere junto al Dealer
(probabilístico, no garantizado en cada cierre - ver §30), no con que el
fix haya "arreglado" el cierre en sí (que sigue sin resolverse, por
diseño - es comportamiento del backend).

---

## 33. Investigación real de la seguridad de lectura/escritura concurrente de mbedTLS - el mutex del §31 sí es viable (2026-07-16)

El usuario preguntó, antes de invertir en el diseño de "hilo de ping
separado" propuesto en §31 (replicar la goroutine independiente de
`go-librespot`), si acortar los intervalos de ping podía probar lo mismo
sin tocar la concurrencia. Se descartó esa vía (ver el intercambio en la
conversación: acortar el intervalo no cierra el hueco *durante* el
bloqueo en sí, solo reduce qué tan viejo es el último ping *antes* de
entrar a él - y ya hay evidencia de §22, con `librespot-rust` pingueando
más lento y nunca sufriendo esto, en contra de que la frecuencia sea la
palanca real). El usuario pidió entonces investigar de verdad la
seguridad de `mbedtls_ssl_context` para lectura/escritura concurrente,
en vez de seguir asumiendo sin verificar como hizo §22.

**Investigado con el código fuente real** (hay una instalación completa
de ESP-IDF v6.0.1 en este entorno,
`/home/user/.espressif/v6.0.1/esp-idf/components/mbedtls/mbedtls/` - no
vendorizada en el repo, pero disponible en el sistema para leer):

1. **`mbedtls_ssl_context` (`include/mbedtls/ssl.h:1577-1777`) separa
   lectura y escritura estructuralmente**: buffers `in_*`
   (`in_buf`/`in_msg`/`in_len`/etc.) y `out_*`
   (`out_buf`/`out_msg`/`out_len`/etc.) son campos completamente
   distintos, con `transform_in`/`transform_out`/`session_in`/
   `session_out` también separados por dirección. Diseño pensado con esa
   separación en mente, no accidental.
2. **Pero `mbedtls_ssl_read()` puede disparar una escritura por dentro**
   (`library/ssl_msg.c:4790` y `:4821`, dentro de
   `ssl_get_next_record()`): un record con MAC inválido llama directo a
   `mbedtls_ssl_send_alert_message()` - que sí toca los buffers `out_*`.
   Confirma que lectura y escritura **comparten estado mutable real** -
   no es solo "no está documentado", hay un camino de código concreto
   que lo demuestra. Dos hilos tocando el mismo `mbedtls_ssl_context` sin
   ningún mutex en absoluto sería inseguro de verdad - la cautela
   original de §22 estaba bien fundada, no era exagerada.
3. **`mbedtls_ssl_write()` no llama al camino de lectura** en la
   práctica para este proyecto (`ssl_msg.c:5850-5877`): la única vía
   sería re-negociación TLS 1.2 (`ssl_check_ctr_renegotiate()`) o
   terminar el handshake si todavía no cerró - ninguna aplica en
   operación normal, porque este proyecto negocia TLS 1.3 (§20 ya lo
   confirma en su título) y el handshake del Dealer termina mucho antes
   de que el ping/pong entre en juego. `renego_status` ni siquiera existe
   en el contexto salvo con `MBEDTLS_SSL_RENEGOTIATION`, una feature
   exclusiva de TLS 1.2.
4. **El bloqueo real de `cspot` durante el procesamiento de un comando
   (`contextResolver.resolve()` + `putBufferingState()`, ~1-2s) nunca
   toca el `mbedtls_ssl_context` del Dealer** - corren sobre sockets TLS
   completamente distintos (`ContextResolver::connection`,
   `PlayerEngine::putConnection`, cada uno su propio
   `bell::TLSSocket`/`mbedtls_ssl_context`). Confirmado leyendo
   `ContextResolver.h`/`PlayerEngine.cpp` - ninguno reutiliza el
   socket de `EspWebSocketTransport`.

**Conclusión, con evidencia real detrás en vez de precaución sin
verificar**: no hace falta lectura/escritura verdaderamente concurrente
sin sincronizar (el caso inseguro, confirmado en el punto 2). Alcanza con
un **mutex liviano que serialice cada llamada individual a
`mbedtls_ssl_read()`/`mbedtls_ssl_write()` sobre el `ssl_context` del
Dealer específicamente** - sostenido solo durante esa llamada puntual
(milisegundos), nunca durante los 1-2s de HTTP bloqueante (que ocurre en
otro `ssl_context` por completo, sin necesitar este mutex para nada). La
única colisión real posible es un ping saliendo justo en el instante en
que el hilo principal está en medio de leer ese mismo socket - ventana
angosta, resuelta trivialmente con un mutex normal, no el problema
general de "concurrencia sin restricciones" que §22 evitó por prudencia.

**Diseño propuesto, no implementado todavía**: un `std::mutex` nuevo en
`EspWebSocketTransport` (o en `bell::TLSSocket` mismo, para que cubra
también a `sendFrame()`/`readExact()`), tomado brevemente alrededor de
cada `socket.read()`/`socket.write()` real - no alrededor de todo
`receiveMessage()`/`sendText()`, que seguirían corriendo como hoy en el
hilo de `DealerClient::runTask()`. Una tarea nueva, separada, con el
único trabajo de: dormir hasta el próximo vencimiento del ping (WS y/o
JSON), tomar el mutex, escribir el frame de ping, soltar el mutex -
exactamente el rol de `pingTicker()` en `go-librespot` (`dealer.go:126`,
§31). El hilo de `DealerClient::runTask()` sigue igual en todo lo demás
(sigue bloqueándose en `contextResolver.resolve()`/`putBufferingState()`
sobre sus propios sockets, sin cambios) - solo dejaría de ser el único
que puede tocar el socket del Dealer.

**Implementado (2026-07-16, mismo día, a pedido del usuario)**:

- **`EspWebSocketTransport.cpp`**: nuevo `std::mutex ioMutex`, tomado en
  los 4 puntos de entrada públicos que tocan el socket
  (`sendText()`/`sendPing()`/`receiveMessage()`/`disconnect()`) - uno a
  la vez, nunca sostenido más allá de una llamada. `sendFrame()`/
  `readExact()`/`writeAll()` quedan como primitivas internas sin lock
  propio - las llamadas a `sendFrame()` hechas desde *dentro* de
  `receiveMessage()` (eco de pong, eco de close, ping proactivo de
  `onIdlePoll()`) reusan el lock que `receiveMessage()` ya sostiene;
  `std::mutex` no es reentrante, así que si `sendFrame()` intentara
  tomarlo de nuevo sería deadlock - por eso el lock vive solo en los
  puntos de entrada, no en los helpers internos. `connect()`/
  `handshake()` quedan sin lock (no hace falta - `DealerPingTask` recién
  se crea después de que `connectOnce()` devuelve true, no puede haber
  concurrencia todavía en esa fase).
  - **Laguna encontrada y corregida durante la implementación**: en el
    primer borrador, `disconnect()`'s `socket.close()` quedaba *fuera*
    del lock (solo el envío del frame de cierre estaba adentro) - eso
    hubiera permitido que `socket.close()` liberara el
    `mbedtls_ssl_context` mientras `DealerPingTask` estuviera a mitad de
    un `write()` sobre el mismo contexto. Corregido: todo el cuerpo de
    `disconnect()` (envío del cierre + `socket.close()`) ahora vive bajo
    el mismo lock.
- **`DealerClient.cpp`**: nueva clase `DealerPingTask` (`bell::Task`
  propio, 16KB de stack - el precedente más chico ya validado en este
  proyecto para algo que toca TLS es `MercurySession` a 16KB;
  `PlayerEngine.cpp` ya documenta que 4KB desbordó haciendo un PUT
  HTTPS inline, así que no se intentó nada más chico sin validar). Manda
  `{"type":"ping"}` cada `JSON_PING_INTERVAL_MS` en su propio ciclo de
  sueño (en pasos de 250ms, para que parar no tenga que esperar el
  intervalo completo), sin depender de que `DealerClient::runTask()` esté
  libre. Se crea justo después de un `connectOnce()` exitoso, se destruye
  (parada + join, mismo patrón F93 ya usado en el resto del proyecto: un
  `std::atomic<bool> running` más un `std::mutex taskLifetimeMutex` que
  el destructor toma para bloquear hasta que `runTask()` lo suelte) antes
  de que `transport` se reemplace en el próximo intento de reconexión -
  el orden importa: si `DealerPingTask` sobreviviera a ese reemplazo,
  terminaría escribiendo sobre un puntero a un transporte ya destruido.
  Se sacó la lógica vieja de ping JSON del loop principal de `runTask()`
  (el `lastJsonPing`/chequeo de intervalo inline) - ahora vive
  exclusivamente en `DealerPingTask`.

**Verificado**: build de host limpio, `unit_tests` 27/27 (112/112
asserts), `f93_concurrency_test`/`f87_logger_concurrency_test` sin
regresión - pero ninguno de los dos archivos tocados
(`EspWebSocketTransport.cpp`, `DealerClient.cpp`) es parte de la suite de
host (el primero está detrás de `#ifdef ESP_PLATFORM`, el segundo no
aparece en `tests/CMakeLists.txt`) - el cambio se verificó por lectura
cuidadosa línea por línea de ambos archivos completos, no por test
automatizado ni build real de ESP-IDF (no disponible en este entorno).

**Pendiente confirmar en hardware real**: el experimento que este cambio
existe para correr - si el cierre del Dealer WS desaparece (o se vuelve
mucho más raro) con el ping JSON fluyendo sin cortes durante el
procesamiento de comandos que cambian de contexto, confirma la hipótesis
de §31 como la causa real (no solo un factor concurrente). Si el cierre
persiste exactamente igual pese al ping continuo, la descarta
definitivamente y deja el fingerprint TLS (§28) como único candidato que
queda en pie - de cualquier manera, un resultado útil. También pendiente
confirmar que no aparecen efectos secundarios nuevos (uso de stack,
contención del mutex bajo tráfico real, cualquier interbloqueo no
anticipado) que solo hardware real puede mostrar.

**Primera prueba en hardware (2026-07-16, mismo día) - el cierre persiste,
y además un crash nuevo**: el usuario probó y reportó "no funcionó,
además crasheó". Dos resultados distintos, cada uno con su propia lectura:

1. **El cierre del Dealer WS sigue exactamente igual** - `play ->
   success=1` seguido, milisegundos después, del cierre. Pero esta vez el
   log trae algo que las corridas anteriores no tenían: un ciclo
   ping/pong completo justo antes. Secuencia exacta:
   `10.077` request `play -> success=1` → `10.110` Mercury GET (arranca
   el fetch de la key) → **`10.112` `received JSON pong`** → `10.126`
   cierre. Un pong llegó **14ms antes** del cierre - la conexión
   claramente no estaba en silencio en ese momento, el ping/pong seguía
   fluyendo con normalidad. Esto es evidencia bastante directa en contra
   de la hipótesis de §31: si el mecanismo real fuera "el cliente queda
   mudo durante el procesamiento del comando y eso dispara el reciclado",
   no debería cerrarse la conexión con un pong recién recibido. Apunta,
   otra vez, a que es un cierre deliberado del lado servidor,
   independiente de la actividad del cliente - reforzando la lectura ya
   sostenida desde §22, no refutándola.
2. **Crash real en hardware** (`Guru Meditation Error: Cache error,
   MMU entry fault`, registros en `0xfefefefe` - patrón clásico de memoria
   ya liberada), justo después de "Dealer: connection lost, reconnecting".
   Causa encontrada leyendo `BellTask.h`: `bell::Task::~Task()` libera el
   stack de FreeRTOS (`heap_caps_free(xStack)`) tan pronto el objeto C++
   se destruye - pero el propio task de FreeRTOS
   (`taskEntryFuncPSRAM`) sigue ejecutando código sobre ese mismo stack
   un rato *después* de que `runTask()` retorna (arma un timer de
   limpieza de 5s para el TCB, llama `vTaskDelete(NULL)`), y el mutex
   F93 que protegía la destrucción se liberaba apenas `runTask()`
   retornaba, no cuando el task de FreeRTOS terminaba de verdad. El
   diseño original de `DealerPingTask` creaba y destruía una instancia
   en **cada reconexión** del Dealer - la primera vez que este proyecto
   crea/destruye un `bell::Task` con esa frecuencia (todos los demás
   viven toda la sesión) - y esa frecuencia fue lo que hizo que la
   condición de carrera preexistente en `bell::Task` se manifestara de
   verdad.

**Corregido, sin tocar `bell::Task` (compartido por todo el proyecto,
más arriesgado de tocar que rediseñar este único uso)**: `DealerPingTask`
ahora se crea **una sola vez**, en el constructor de `DealerClient`, con
la misma vida que el resto de los tasks del proyecto (se destruye recién
al final de la sesión, nunca por reconexión). En vez de poseer el
`WebSocketTransport` directamente, ahora lee un puntero no-propietario
(`DealerClient::activeTransport`) protegido por
`DealerClient::activeTransportMutex` - `connectOnce()` lo limpia (bajo
el mismo mutex, bloqueando hasta que el ping task no esté a mitad de un
envío) antes de reemplazar/destruir `transport`, y lo vuelve a setear
recién tras una conexión exitosa. Orden de declaración de miembros
corregido en `DealerClient.h` (`activeTransportMutex`/`activeTransport`
antes que `pingTask`) para que la destrucción en reversa pare el hilo del
ping *antes* de que sus propias referencias a esos dos miembros dejen de
ser válidas.

**Verificado**: build de host limpio, `unit_tests` 27/27, 112/112
asserts sin regresión (de nuevo, ninguno de los dos archivos es parte de
esa suite - verificado por lectura completa y cuidadosa, no por test
automatizado). **Pendiente**: confirmar en hardware real que (a) el crash
no se repite con el nuevo ciclo de vida, y (b) reunir una corrida más
donde el cierre pase justo después de un ciclo ping/pong reciente (como
esta) para terminar de asentar que la hipótesis de §31 queda descartada.

---

## 34. Comparación campo por campo del PUT contra `go-librespot` - `has_been_playing_for_ms` faltaba (2026-07-16)

El usuario preguntó si el problema era un PUT mal formado (a comparar
contra `go-librespot`) o el handshake, "ya que lo estamos reciclando".
Se comparó campo por campo la construcción completa del PUT real
(`daemon/player_state.go`'s `putConnectState()` - la referencia
completa, no solo lo ya visto en `dealer.go`) contra
`PlayerEngine::buildDeviceInfo()`/`sendPutStateRequest()`.

**Diferencias encontradas en `Capabilities`**:
- `supports_logout`: go-librespot lo manda (atado a si zeroconf logout
  está habilitado), `cspot` nunca lo toca (queda `false`).
- `supports_gzip_pushes`: ya conocida y deliberada (§6.3) - `cspot` la
  manda `false` a propósito, go-librespot `true`.
- `restrict_to_local`/`supports_set_backend_metadata`/
  `supports_set_options_command`: go-librespot los usa, pero **ni
  siquiera existen** en el `.proto` recortado de `cspot` - falta el campo
  entero, no solo setearlo.

**Diferencia encontrada en `PutStateRequest`, la más concreta**:
`has_been_playing_for_ms` (campo 11) **existe en el propio `.proto`
recortado de `cspot`** (`connectstate.proto`), pero
`sendPutStateRequest()` nunca lo llenaba - a diferencia de
`started_playing_at`/`activeSinceMs` (device-level, "desde cuándo está
activo el dispositivo"), este es track-level: cuánto tiempo lleva
reproduciendo el track actual, análogo al `Player.startedPlaying` de
`go-librespot` (`player/player.go:373`, seteado en cada
`SetPrimaryStream()`, pausado o no).

**Sobre el handshake**: no se encontró una discrepancia estructural -
los headers del handshake WS ya coinciden desde §27, y la reconexión de
`go-librespot` no hace nada especial con el `connection-id` que `cspot`
no replique ya. Sin evidencia concreta, no se tocó nada ahí - el usuario
decidió priorizar `has_been_playing_for_ms` únicamente por ahora.

**Implementado**: nuevo miembro `PlayerEngine::
currentTrackStartedAtMs` (guardado por `engineMutex`, mismo patrón que
`positionMs`/`isPlayingState`), seteado en `trackLoadedCallback()`
(constructor) cada vez que un track real carga - mismo momento que
`positionMeasuredAt`. `sendPutStateRequest()` calcula
`request.has_been_playing_for_ms = getSyncedTimestamp() -
currentTrackStartedAtMs` cuando este es distinto de 0, junto al cálculo
ya existente de `started_playing_at`/`activeSinceMs`.

**Verificado**: build de host limpio, `unit_tests` 27/27, 112/112
asserts sin regresión. `PlayerEngine.cpp`/`.h` no son parte de la
suite de host - verificado por lectura, confirmando además el nombre y
tipo exacto del campo generado por nanopb
(`build/esp-idf/cspot/cspot/protobuf/connectstate.pb.h`:
`uint64_t has_been_playing_for_ms`, sin flag `has_*` propio al ser un
escalar proto3). **Pendiente confirmar en hardware real**: no se espera
que esto por sí solo resuelva el cierre del Dealer WS (nada en la
investigación hasta ahora sugiere que el backend dependa de este campo
específico para decidir reciclar la conexión) - es una corrección de
fidelidad hacia la referencia, no una hipótesis de causa raíz nueva.

---

## 35. Revertido el experimento del hilo de ping separado (§33) - el cierre persiste (2026-07-16)

El usuario probó §33 en hardware: "no funcionó, además crasheó" (el
crash se investigó y corrigió aparte, ver el punto 2 de la nota agregada
a §33 arriba). Tras esa corrección, una segunda prueba mostró el mismo
patrón de siempre - `play -> success=1` seguido del cierre, con un
`pong` recibido apenas 14ms antes. Con el mecanismo que este experimento
existía para probar (ping ininterrumpido durante el procesamiento de un
comando) confirmado funcionando y el cierre pasando de todos modos, el
usuario pidió sacar el código del experimento - no aporta nada y suma
complejidad/riesgo (una clase de task nueva, un mutex, una condición de
carrera de `bell::Task` que hubo que aprender a mano).

**Revertido por completo**: `EspWebSocketTransport.cpp` vuelve al diseño
de un solo hilo sin `ioMutex` (comentario de cabecera actualizado con una
nota breve de qué se probó y por qué se volvió atrás, en vez de borrar
el rastro). `DealerClient.h`/`.cpp` vuelven exactamente al estado antes
de §33 vía `git checkout` (diffs aislados, verificados con `git diff`
antes de revertir) - sin `DealerPingTask`, sin
`activeTransport`/`activeTransportMutex`, el ping JSON vuelve a vivir
inline en `runTask()` como antes.

**Verificado**: build de host limpio, `unit_tests` 27/27, 112/112
asserts sin regresión.

**Estado de la investigación tras esto**: con el hilo separado
descartado como palanca (el cierre persiste con o sin él, y con o sin
ping ininterrumpido), lo único que sigue en pie de §22-§34 es: (a) el
fingerprint TLS (§28, no confirmable sin captura de paquetes), y (b)
seguir cerrando huecos de fidelidad hacia `go-librespot` uno por uno
(§32/§34/§36) por si alguno resulta ser lo que el backend efectivamente
está esperando para no reciclar la conexión - sin garantía, pero es la
única vía que queda barata de seguir probando.

---

## 36. Fidelidad de protocolo con `go-librespot` (versión estable, tag `v0.7.4`) - `prev_tracks`/`next_tracks` y `context_restrictions`/`context_metadata` (2026-07-16)

A pedido del usuario: seguir revisando la versión **estable** de
`go-librespot` (no la punta de `master`, que tiene 7 commits sin
release desde `v0.7.4` - confirmado con `git diff v0.7.4 master`, la
única diferencia real en los archivos ya comparados es una función de
coalescing de PUTs bajo rate-limit, no relevante acá) y verificar qué
protos faltan, tratando de ser fiel a su lógica real.

**Comparación completa**: `proto/spotify/connectstate/connect.proto`
(266 líneas) + `player.proto` (184 líneas) de `go-librespot` contra el
`connectstate.proto` recortado de `cspot` (240 líneas antes de este
cambio). El grueso de lo que falta es cosmético/fuera de alcance para un
receptor headless (hifi, DJ, rooms, sleep timer, ads, dispositivos
"dodo") - filtrado comparando contra qué campos **el propio código de
`go-librespot` lee o escribe de verdad** (`grep` en `daemon/*.go`), no
solo qué existe en el `.proto`.

**Cuatro huecos reales encontrados** (`docs/dealer_websocket_migration.md`,
turno anterior de esta conversación): `supports_logout` (no aplicado),
`prev_tracks`/`next_tracks` (nunca enviados), `context_restrictions`/
`context_metadata` (lo que `update_context` realmente actualiza -
`player.go:370-375`), `play_origin` y `suppressions`. El usuario eligió
implementar `prev_tracks`/`next_tracks` y `context_restrictions`/
`context_metadata` en este turno.

**`connectstate.proto`/`.options` - nuevos mensajes/campos** (validado
generando de verdad con `protoc`+`nanopb_generator.py` en un venv
temporal del sandbox, no solo por lectura - `nanopb_generator.py`
necesitó `protobuf`+`setuptools<81` para correr, ver nota de
`pkg_resources` deprecado en setuptools reciente):
- `Restrictions` (mensaje nuevo): recortado a los 3 motivos que
  `go-librespot` realmente lee (`daemon/controls.go`'s
  `handleSetOptions()`) de los ~25 que tiene el schema real -
  `disallow_toggling_repeat_context_reasons`/`_repeat_track_reasons`/
  `_shuffle_reasons`, y cada uno simplificado de `repeated string` a
  `string` (go-librespot solo mira "¿la lista está vacía o no?", nunca
  enumera motivos - un string no vacío transmite la misma señal sin la
  contabilidad de `max_count`/`max_size` de un campo repetido).
  `max_size:48` cada uno.
- `PlayerState.context_restrictions` (campo 4, tipo `Restrictions`).
- `PlayerState.prev_tracks`/`next_tracks` (campos 19/20, `repeated
  ProvidedTrack`) - go-librespot cachea hasta 32
  (`tracks/tracks.go:MaxTracksInContext`), acá `max_count:3` cada uno -
  32 `ProvidedTrack` completos (con sus 5 campos string) hubiera costado
  ~13KB por array en el struct de codificación (que vive en el stack de
  `sendPutStateRequest()`), demasiado para el presupuesto de stack de
  esta tarea.
- `PlayerState.context_metadata` (campo 21, `map<string,string>`) -
  nanopb lo desazucara en un mensaje `PlayerState_ContextMetadataEntry`
  con `key`/`value` - confirmado generando de verdad, no adivinado (el
  primer intento con el patrón de opciones equivocado
  -`connectstate.PlayerState.context_metadata.key`- no matcheó nada y
  generó punteros `FT_POINTER` por default; corregido a
  `connectstate.PlayerState.ContextMetadataEntry.key`, el nombre real
  del tipo anidado). `max_count:2`, `max_size:32` para `key`/`value`.

**`TrackQueue`**: nuevos `getPrevTracks(maxCount)`/`getNextTracks(maxCount)`
- misma convención que `tracks.List.PrevTracks()/NextTracks()` de
  `go-librespot`, pero contra `currentTracks`/`currentTracksIndex` (la
  lista de contexto ya resuelta) en vez de un iterador de contexto
  perezoso. Guardado por `tracksMutex`, mismo patrón que el resto de la
  clase.

**`PlayerEngine`**: `handlePlayerCommand()`'s caso `update_context`
ahora parsea `command.context.restrictions`/`command.context.metadata`
del JSON real (antes solo comparaba el uri y re-mandaba el PUT sin
aplicar el contenido - la mitad del fix de §32) y los cachea en tres
miembros nuevos (`restrictionRepeatContext`/`_repeatTrack`/`_shuffle`) +
`contextMetadata` (guardados por `engineMutex`).
`sendPutStateRequest()` los vuelca al PUT real junto con
`prev_tracks`/`next_tracks` (vía `trackQueue->getPrevTracks(3)`/
`getNextTracks(3)`, solo `uri` poblado - `TrackReference` no tiene
`uid`/`provider`/`album_uri`/`artist_uri` de dónde sacar el resto).

**Verificado**: el `.proto`/`.options` se generó de verdad con
`protoc`+`nanopb_generator.py` (no solo el build de ESP-IDF, no
disponible acá) - sin warnings, structs con arrays estáticos como se
esperaba (no `FT_POINTER`), consistente con el resto del archivo
("Encode-only messages: static arrays, stack-friendly"). Build de host
limpio, `unit_tests` 27/27, 112/112 asserts sin regresión - ninguno de
`PlayerEngine.cpp`/`.h`/`TrackQueue.cpp`/`.h` es parte de esa
suite (`TrackQueue.cpp` sí compila para host en otros contextos, pero no
está en `tests/CMakeLists.txt`) - verificado por lectura completa y
cuidadosa de cada archivo tocado, con especial atención a los tipos
exactos generados por nanopb (confirmados corriendo el generador, no
adivinados). **Pendiente confirmar en hardware real**: build de ESP-IDF
limpio (no disponible en este entorno), que el PUT resultante no exceda
ningún límite de tamaño de spclient, y que la app muestre la cola/las
restricciones correctamente. Igual que §34, no se espera que esto por sí
solo resuelva el cierre del Dealer WS - es fidelidad hacia la
referencia, no una hipótesis de causa raíz nueva.

---

## 37. Bug real encontrado en hardware por §36: `pbPutString()` sin bounds-check desbordaba con datos externos - corregido (2026-07-16)

El usuario trajo un log real con algo nunca visto antes: `connect-state
PUT failed, status 400: {"error_type":"INVALID_ENTITY","message":"Invalid
protobuf"}`, repetido varias veces, empezando justo después de que un
`update_context` real trajera contenido (no vacío) en
restrictions/metadata - exactamente el camino nuevo de §36.

**Causa**: `pbPutString()` (`NanoPBHelper.cpp`, helper preexistente, no
tocado por §36) no valida el tamaño del destino - copia
`stringToPack.size()` bytes sin importar la capacidad real del `char[N]`.
Todo uso previo en este archivo pasa strings generados internamente ya
acotados (session_id, URIs propias). §36 fue la primera vez que este
helper recibió strings de **contenido arbitrario del servidor**
(razones de restricción, valores de metadata de un `update_context`
real) - si cualquiera superaba los 48/32 caracteres configurados en
`connectstate.options`, la escritura se iba fuera del array y corrompía
el campo siguiente del struct, produciendo protobuf inválido. Al no
limpiarse nunca `restrictionRepeatContext`/`contextMetadata` tras
usarlos, el dato corrupto quedaba pegado y hacía fallar **todos** los
PUTs siguientes (de ahí los varios "Invalid protobuf" seguidos en el
log, uno por cada evento `TRACK_INFO`/`PLAY_PAUSE` que intentó mandar
estado mientras cargaban los tracks siguientes).

**Corregido**: nuevo `truncateForPb(s, maxLen)` en
`PlayerEngine.cpp`, aplicado a las 3 razones de restricción
(`maxLen=47`) y a cada clave/valor de metadata (`maxLen=31`) - el -1 en
cada caso deja lugar al terminador nulo que `pbPutString()` escribe. No
se tocó `pbPutString()` en sí (helper compartido por todo el proyecto,
más arriesgado de cambiar que acotar los dos call sites nuevos que
realmente reciben datos externos). `ProvidedTrack.uri` (prev_tracks/
next_tracks, también nuevo de §36) se dejó sin truncar a propósito -
viene de `TrackReference::uri`, generado internamente con formato fijo
(`spotify:track:<22 chars>`), mismo patrón ya aceptado en `ps.track.uri`
en el resto del archivo - no arbitrario, mismo tipo de dato que ya se
usaba sin este resguardo desde antes de §36.

**Verificado**: build de host limpio, `unit_tests` 27/27, 112/112
asserts sin regresión. **Pendiente confirmar en hardware real** que un
`update_context` con restrictions/metadata reales ya no produzca
"Invalid protobuf".

## 38. ~17s entre el tap de play y la llegada del comando real - hallazgo separado, no corregible desde el cliente (2026-07-16)

El usuario probó de forma controlada: observó el instante exacto en que
tocaba "play" en una playlist nueva, y coincidía exactamente con el
mensaje `hm://playlist/v2/playlist/...` en el log
(`DealerClient.cpp:273`, tipo `"message"`, "unhandled in 5.2"). Confirmó
explícitamente que tocó el botón de play de la playlist directo, no solo
la abrió.

**Lo encontrado**: entre ese mensaje y el comando real
(`hm://connect-state/v1/player/command`, `endpoint=play`) pasan **~17
segundos reales**, con **absolutamente nada más** llegando por el
Dealer en el medio - solo tráfico rutinario de Mercury/AP (ping/pong del
protocolo AP, sin relación con la playlist). Descartado que sea algo de
`cspot`: `DealerClient::handleMessage()` procesa el mensaje
`hm://playlist/v2/...` de forma instantánea (un solo log, sin bloquear
nada) - el loop de lectura de `runTask()` sigue polleando con
normalidad todo ese tramo, no hay nada de nuestro lado que retrase la
entrega del comando siguiente.

**Conclusión**: el propio backend de Spotify tarda ~17s en despachar el
comando de reproducción hacia este dispositivo específico, después de
que el usuario ya tocó play. No es un bug de `cspot` - no hay ninguna
llamada ni bloqueo de nuestro lado entre esos dos mensajes que explique
la demora, y no tenemos forma de acelerar cuánto tarda el servidor en
mandarnos algo. Podría estar relacionado con el mismo mecanismo que
decide reciclar el Dealer WS tras un rato de idle (§22-§36) - quizás el
mismo camino interno que trata a una conexión Dealer "vieja"/idle de
forma especial también retrasa el despacho de comandos nuevos hacia
ella - pero es una hipótesis, no confirmable sin visibilidad del lado
servidor. Queda documentado como hallazgo separado, no como parte de la
cadena de causa del cierre en sí.

---

## 39. `playback_id` codificado distinto al de `go-librespot` - hex vs. base64 (2026-07-16)

El usuario preguntó si alguno de los IDs aleatorios que mandamos podía
tener algo que ver (con el cierre del WS, el retraso de 17s, o la
interacción con Mercury). Se descartó `device_id` primero (no es
aleatorio - hash determinístico del nombre del dispositivo,
`LoginBlob.cpp:23-25`, estable entre sesiones). El usuario preguntó
puntualmente por `playback_id`.

**Encontrado**: `go-librespot` genera 16 bytes aleatorios y los codifica
en **hex** (`daemon/controls.go:399`,
`p.state.player.PlaybackId = hex.EncodeToString(p.primaryStream.PlaybackId)`
- los bytes en sí vienen de `player/player.go:590-591`,
`rand.Read(playbackId)`). `cspot`
(`PlayerEngine.cpp::notifyAudioReachedPlayback()`) generaba los
mismos 16 bytes aleatorios pero los codificaba en **base64**
(`Crypto::base64Encode`) - una divergencia real de la referencia.
Verificado que no es un patrón general: `session_id` sí coincide entre
ambos (los dos usan base64 -
`daemon/controls.go:247`/`player.go:213`, `base64.StdEncoding`) - la
divergencia es específica de `playback_id`.

**Corregido**: `playbackId` ahora usa `bytesToHexString()` (`Utils.h`,
ya usado en otras partes del proyecto - p. ej.
`TrackQueue.cpp`'s `identifier`) en vez de `Crypto::base64Encode`.

**Verificado**: build de host limpio, `unit_tests` 27/27, 112/112
asserts sin regresión. `PlayerEngine.cpp` no es parte de la suite
de host - verificado por lectura, confirmando que `bytesToHexString()`
toma exactamente el tipo que devuelve
`generateVectorWithRandomData()` (`std::vector<uint8_t>`). **No hay
evidencia de que esto explique el cierre del Dealer WS o el retraso de
17s** (`playback_id` es solo un identificador de stream, no algo que el
protocolo de conexión/comandos consulte) - es una corrección de
fidelidad hacia la referencia, en la misma línea que §34/§36, no una
hipótesis de causa raíz nueva. Pendiente confirmar en hardware real.

---

## 40. `go-librespot` real, corriendo en vivo, 20+ minutos sin cierre atravesando el mismo patrón que siempre cierra a `cspot` (2026-07-16)

Detalle completo en `docs/go_librespot_ws_handshake_debug.md` - resumen
acá. Se compiló y corrió un `go-librespot` real (cuenta real del
usuario, emparejado por Zeroconf, reproduciendo audio de verdad vía
`aplay` sobre un pipe), con logging agregado en `dealer/dealer.go` para
capturar el handshake HTTP crudo del WebSocket del Dealer (headers de
la request saliente y la respuesta, con el `access_token` redactado).

**Handshake comparado contra `EspWebSocketTransport.cpp::handshake()`**:
dos diferencias reales de bytes (orden de headers, y casing -
`Sec-Websocket-Key` de Go vs `Sec-WebSocket-Key` de `cspot`) - ninguna
con mecanismo causal plausible (HTTP es case-insensitive e indiferente
al orden de headers por spec), y la respuesta del servidor es idéntica
(`101 Switching Protocols`) en ambos casos.

**Resultado más importante**: se dejó la sesión corriendo 20+ minutos,
atravesando varios cambios de track automáticos, un `skip_next` real, y
- después de varios minutos de idle - un `play` con cambio de contexto
real (cambio de playlist) - exactamente el patrón idle-largo +
comando-que-cambia-contexto que en `cspot` dispara el cierre de forma
confiable desde §22. **La conexión del Dealer de `go-librespot` nunca se
cerró** - un solo handshake en todo el log, ningún
`"dealer connection closed"` posterior. Reafirma con mucha más evidencia
el A/B puntual de §27 (una sola prueba en su momento) - no es
coincidencia de una corrida aislada.

**Estado de la investigación**: con el handshake HTTP ya comparado (sin
diferencias causales plausibles) y el ping/pong ya descartado
(§31/§33/§35), lo único que sigue en pie sin poder confirmarse desde el
código es el **fingerprint TLS del `ClientHello`** (§28) - la capa que
queda por debajo de todo lo ya comparado hasta acá, y la única para la
que haría falta una captura de paquetes real (tcpdump/mitmproxy,
comparando el `ClientHello` de mbedTLS en el ESP32 contra el de
`crypto/tls` de Go byte a byte) para confirmar o descartar
definitivamente.

---

## 41. Captura real del `ClientHello` de `go-librespot` + hallazgo mayor: `cspot` corre solo TLS 1.2, la referencia negocia TLS 1.3 (2026-07-16)

Detalle completo en `docs/go_librespot_ws_handshake_debug.md`. Con
`sudo tcpdump` (corrido por el usuario, no por Claude - sin permisos de
captura cruda en este sandbox) se capturó el `ClientHello` real de
`go-librespot` hacia `guc3-dealer.spotify.com:443`. Parseado a mano en
Python (sin `tshark` disponible acá) - 13 cipher suites (10 ECDHE-AEAD
TLS 1.2 + 3 TLS 1.3), sin ALPN, con `supported_versions` prefiriendo TLS
1.3 sobre 1.2.

**Hallazgo mayor, no cosmético**: el `sdkconfig` real de `cspot` tiene
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 is not set` - **corre solo TLS 1.2**.
Esto ya se había probado antes (§20, por latencia de handshake, no por
esta investigación) y se revirtió tras una falla real en hardware
("session failed: Connection closed while reading HTTP response",
justo después de validar el certificado) - pero esa falla pasó durante
el **bootstrap de la sesión** (AP/Login5), **nunca se llegó a probar
contra el Dealer en sí**. Mucho cambió del lado `cspot` desde entonces
(`SO_SNDTIMEO`, ajustes de ping, `X509Bundle`) que podría explicar por
qué esa falla original ya no reproduce.

**Comparación de cipher suites** (inferida del `sdkconfig` real contra
`ssl_ciphersuites.c` de mbedTLS, no capturada - sin hardware para
capturar el lado `cspot`): con `RSA`/`ECDHE_RSA`/`ECDHE_ECDSA`
habilitados, `GCM`/`CCM` sí, `CHACHA20`/`CAMELLIA`/`ARIA` no, `cspot`
ofrecería del orden de 15-20+ cipher suites TLS 1.2 - incluyendo CBC-SHA
legacy y RSA puro (sin forward secrecy) - contra los 13, cortos y
modernos, de `go-librespot`. Diferencia de fingerprint real y marcada,
del tipo que sistemas de fingerprinting tipo JA3 usan para distinguir
un stack TLS embebido/genérico de un cliente curado - especulativo como
causa del cierre, pero mucho más de peso que el orden/casing de headers
HTTP ya comparado.

**Corrección de rumbo importante, en vivo durante esta prueba**: la
misma conexión de `go-librespot` capturada en el handshake se cerró de
verdad a los ~8.4 minutos - pero durante **idle puro, sin ningún
comando** de por medio (a diferencia del patrón de `cspot`, que nunca
se cierra en idle puro, solo a milisegundos de un comando que cambia de
contexto). Lectura: probablemente dos fenómenos distintos - un
reciclado genérico por idle que ambos clientes sufren por igual y
ninguno puede evitar, y el patrón específico/determinístico de `cspot`
(cierre inmediato tras un comando) que la referencia nunca mostró en
20+ minutos con varios comandos reales de por medio. Detalle completo
en `go_librespot_ws_handshake_debug.md`.

**Implementado - segundo intento de TLS 1.3 en hardware**:
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` en `sdkconfig.defaults` (con el
razonamiento completo de por qué reintentar, en un comentario) y en
`sdkconfig` directamente (un `idf.py reconfigure` solo no alcanza para
subir una opción que el `sdkconfig` ya generado tiene resuelta
explícitamente en "not set" - mismo problema, en sentido inverso, que
§20 ya documentó al bajarla). **No verificable en este entorno** - no
hay toolchain de ESP-IDF acá, hace falta que el usuario compile
(`idf.py build`), flashee, y prueba en hardware real. Si vuelve a fallar
en el bootstrap de la sesión (mismo síntoma de §20: "Connection closed
while reading HTTP response" justo tras validar el certificado),
revertir ambos archivos a "is not set". Si el bootstrap conecta bien
esta vez, el siguiente paso es confirmar específicamente que el Dealer
también negocia TLS 1.3 sin problema (algo que nunca se llegó a probar
en el primer intento) y observar si el patrón de cierre post-comando
cambia.

**Candidato concreto para la falla original, encontrado leyendo la doc
oficial de ESP-TLS** (`docs.espressif.com/.../esp_tls.html`) mientras se
compilaba este segundo intento: en TLS 1.3, **los `NewSessionTicket` del
servidor pueden llegar en cualquier momento después del handshake**, no
solo al principio como en TLS 1.2 - "session tickets may arrive from the
server at any point after the handshake". El `sdkconfig` de `cspot`
tiene `CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=y` y los key-exchange
modes PSK de TLS 1.3 habilitados - los tickets están activos a nivel
mbedTLS aunque `cspot` no use la capa `esp_tls` (usa `bell::TLSSocket`
directo). `EspWebSocketTransport`/`TLSSocket::read()` (§22) se diseñó
pensando únicamente en semántica TLS 1.2 - nunca contempló que un
registro no-de-aplicación pudiera llegar de sorpresa en medio de una
espera de datos con el mecanismo de timeout propio de este proyecto.
Candidato concreto y plausible para el "Connection closed while reading
HTTP response" de §20 - **si TLS 1.3 vuelve a fallar, probar
`CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=n` como variable aislada
antes de descartar TLS 1.3 por completo.**

---

## 42. Confirmado y corregido: el bug de TLS 1.3 era exactamente el `NewSessionTicket` post-handshake de mbedTLS 4.0 (2026-07-16)

Segunda prueba en hardware con `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y`
(§41): **reprodujo el mismo síntoma exacto** de §20 -
`session failed: Connection closed while reading HTTP response`, justo
después de `esp-x509-crt-bundle: Certificate validated`. El usuario pidió
revisar las clases de `bell` y comparar contra
`esp-tls_mbedtls.c` real de ESP-IDF.

**Causa raíz confirmada**, leyendo el header real de mbedTLS
(`include/mbedtls/ssl.h:83-86`):

```c
/**
 * Received NewSessionTicket Post Handshake Message.
 * This error code is experimental and may be changed or removed without notice.
 */
#define MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET       -0x7B00
```

Coincide exactamente con lo que advertía la doc oficial de ESP-TLS leída
en el turno anterior: en TLS 1.3, el servidor puede mandar un
`NewSessionTicket` en cualquier momento después del handshake - acá,
literalmente el instante siguiente (`sdkconfig` ya tenía
`CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=y` habilitado, sin que
`cspot` lo supiera o lo pidiera explícitamente). mbedTLS 4.0, al recibir
uno, hace que `mbedtls_ssl_read()` devuelva este código en vez de
consumirlo en silencio y seguir esperando datos de aplicación como
haría un wrapper TLS "de más alto nivel" (el propio `esp_tls_mbedtls.c`
de ESP-IDF, revisado como pidió el usuario, maneja esto explícitamente -
ver más abajo). **`bell::TLSSocket::read()` no conocía este código** -
como cualquier otro valor negativo, caía al mismo camino que un error
fatal real: `timedOut`/`peerClosedCleanly` quedaban ambos en `false`,
devolvía 0 bytes, indistinguible de una conexión muerta para
`SocketStream`/`HTTPClient::readResponseHeaders()` (`HTTPClient.cpp:149`,
el `throw` exacto del síntoma observado).

**Confirmado contra `esp_tls_mbedtls.c` real** (leído de verdad en
GitHub, como pidió el usuario - no asumido): `esp_mbedtls_read()` tiene
manejo explícito para exactamente este código:

```c
if (mbedtls_ssl_get_version_number(&tls->ssl) == MBEDTLS_SSL_VERSION_TLS1_3) {
    while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
           tls->ssl.MBEDTLS_PRIVATE(state) == MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET) {
        ESP_LOGD(TAG, "got session ticket in TLS 1.3 connection, retry read");
        // (guarda el ticket si CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS)
        ret = mbedtls_ssl_read(&tls->ssl, (unsigned char *)data, datalen);
    }
}
```

Mismo patrón que el fix aplicado acá, con una diferencia: ESP-IDF
también revisa el estado interno de la máquina de estados de mbedTLS
(`ssl.state`, vía `MBEDTLS_PRIVATE` - acceso a un campo del struct que
mbedTLS considera privado) como chequeo extra, cubriendo el caso de un
ticket que llegue fragmentado entre lecturas. El fix de `bell::TLSSocket`
sólo revisa el código de retorno documentado (la API pública, no la
estructura interna) - más simple y no depende del layout interno de
mbedTLS, a costa de ser potencialmente menos robusto ante ese caso
fragmentado específico (no confirmado si aplica en la práctica al
tamaño típico de un `NewSessionTicket`). `bell::TLSSocket` es un wrapper
mucho más fino/directo sobre mbedTLS que `esp_tls` (sin pasar por esa
capa) y nunca había necesitado saber esto porque nunca antes había
corrido con TLS 1.3 activo el tiempo suficiente para toparse con un
ticket real.

**Corregido**: `TLSSocket::read()` (`bell/main/io/TLSSocket.cpp`) ahora
reintenta `mbedtls_ssl_read()` en un loop mientras el resultado sea
`MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET` - el ticket ya está
procesado/guardado internamente por mbedTLS en ese punto, así que
reintentar de inmediato es lo correcto (no hace falta resetear ningún
timeout, es una re-entrada síncrona sobre datos ya en el buffer, no una
espera nueva de red). Aislado a esta única función - si una versión
futura de mbedTLS cambia o quita este código (está marcado
"experimental" en su propio header), solo hay que tocar acá.

**Verificado**: build de host limpio - `TLSSocketStub.cpp` (usado en
tests) no necesitó tocarse, no implementa lectura real. No verificable
más allá de eso en este entorno (`TLSSocket.cpp` es `#ifdef
ESP_PLATFORM`, no compila para host) - pendiente que el usuario
recompile y pruebe en hardware real. Si el bootstrap de la sesión
conecta bien esta vez, el siguiente paso sigue siendo el mismo de §41:
confirmar que el Dealer también negocia TLS 1.3 sin problema y observar
si el patrón de cierre post-comando cambia.

## 43. El fix de §42 no alcanzaba: faltaba el chequeo de `ssl.state`, no solo el código de retorno (2026-07-16)

**Reproducido**: hardware retesteado con el fix de §42 aplicado -
**mismo error exacto** (`session failed: Connection closed while
reading HTTP response`, justo tras "Certificate validated"). El fix de
§42 no estaba mal, estaba incompleto: quedó documentado ahí mismo como
una decisión consciente de *no* replicar el chequeo extra de
`esp_mbedtls_read()` sobre `ssl.MBEDTLS_PRIVATE(state)` - "a costa de
ser potencialmente menos robusto ante ese caso fragmentado específico
(no confirmado si aplica en la práctica)". Esa decisión resultó
incorrecta: el caso sí aplica en la práctica.

**Causa real**: releída la fuente real de `esp_mbedtls_read()`
(`esp_tls_mbedtls.c`, vía WebFetch) con más atención al *loop
condition* exacto, no solo a qué código de error dispara el retry:

```c
while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
       tls->ssl.MBEDTLS_PRIVATE(state) == MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET) {
    ESP_LOGD(TAG, "got session ticket in TLS 1.3 connection, retry read");
    ret = mbedtls_ssl_read(&tls->ssl, (unsigned char *)data, datalen);
}
```

Es un **OR de dos condiciones independientes**, no una sola. El fix de
§42 sólo cubría la primera mitad (`ret ==
MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET`). La segunda mitad existe
porque mbedTLS puede dejar la máquina de estados de TLS 1.3 parada en
`MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET` y hacer que esa llamada a
`mbedtls_ssl_read()` devuelva un **0 llano** (no el código negativo
especial) mientras sigue en ese estado intermedio. Un `0` es
indistinguible, para cualquier caller de `bell::TLSSocket::read()`, de
un EOF/conexión cerrada:

- `SocketStream::underflow()` (`bell/main/io/SocketStream.cpp`): `br <=
  0` → `setg(NULL, NULL, NULL)` → EOF, sin distinguir "cerrado de
  verdad" de "0 bytes legítimos this vez".
- Eso deja el `std::istream` en `failbit` en la siguiente lectura de
  `HTTPClient::readResponseHeaders()`'s `getline()`, que lanza
  exactamente `"Connection closed while reading HTTP response"`
  (`HTTPClient.cpp:149`).

Es decir: el `ret < 0` que el fix de §42 vigilaba nunca ocurría en este
caso concreto - `mbedtls_ssl_read()` volvía con `0`, mi loop
`while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)` lo veía
como "no hay más ticket que consumir" y salía inmediatamente,
devolviendo 0 bytes al caller como si la conexión estuviera cerrada.

**Corregido**: `TLSSocket::read()` ahora replica la condición completa
de `esp_mbedtls_read()`:

```cpp
do {
  ret = mbedtls_ssl_read(&ssl, buf, len);
} while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
         ssl.MBEDTLS_PRIVATE(state) == MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET);
```

`ssl.MBEDTLS_PRIVATE(state)` es accesible sin agregar
`CONFIG_MBEDTLS_ALLOW_PRIVATE_ACCESS` ni nada al `sdkconfig`: se
confirmó leyendo `private_access.h` de mbedTLS (vendorizado en
`components/mbedtls/mbedtls/` del propio ESP-IDF v6.0.1) que
`MBEDTLS_PRIVATE(member)` es *solo* una convención de nombres - sin ese
define expande a `private_##member` (ej. `private_state`), pero el
campo sigue siendo un miembro real y accesible de la struct, no algo
oculto por el compilador. `esp_tls_mbedtls.c` usa exactamente el mismo
patrón sin requerir ese define tampoco.

**Verificado**: `ssl` es un `mbedtls_ssl_context` de verdad (no un
puntero opaco) en `TLSSocket.h`, y `MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET`
existe como valor del enum de estados en
`components/mbedtls/mbedtls/include/mbedtls/ssl.h:729` de la copia
vendorizada por ESP-IDF v6.0.1 instalada localmente - confirmado leyendo
el archivo directamente, no asumido. No compilable en host (mismo
motivo de siempre, `TLSSocket.cpp` es `#ifdef ESP_PLATFORM`) - pendiente
prueba en hardware real. Si el bootstrap conecta esta vez, sigue
pendiente el objetivo original de §41: confirmar si el Dealer con TLS
1.3 activo cambia el patrón de cierre post-comando.

**Confirmado en hardware real**: el bootstrap de sesión (AP + Login5 +
`apresolve.spotify.com`) conecta y autentica correctamente con
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` activo - el fix era efectivamente
este. Pendiente el objetivo original de §41: dejar el dispositivo en
idle unos minutos y luego mandar un comando (play/transfer) para
confirmar si el Dealer WS - ahora también negociando TLS 1.3 - sigue
cerrándose justo después del comando, o si el patrón cambió.

**Resultado, y descartado como causa**: probado en hardware real -
idle unos minutos, luego un comando (play/transfer) - **el cierre
sigue ocurriendo exactamente igual** que con TLS 1.2. Con esto, TLS 1.3
queda **descartado** como causa del cierre del Dealer WS post-comando:
era una hipótesis razonable (era la diferencia estructural más grande
encontrada frente a `go-librespot` en §41) pero el experimento real la
refuta. El fix de esta sección (§42/§43) sigue siendo válido y se
mantiene (corrige un bug real y reproducible del bootstrap de sesión
con TLS 1.3 activo, e ir de vuelta a solo TLS 1.2 no tiene ya ningún
motivo funcional), pero no resuelve el problema que motivó toda esta
investigación (§20 en adelante). Queda descartado de la lista de
sospechosos: versión de TLS, cifrado/ALPN, orden de headers HTTP,
diseño del ping (ambas variantes), `playback_id`, overflow de buffer en
campos nuevos del proto. La causa raíz del cierre sigue sin
identificarse.

## 44. Hipótesis "conexión zombie" del usuario + hallazgo concreto: cspot nunca detectaba pongs perdidos, a diferencia de `go-librespot`

**Hipótesis del usuario**: la conexión probablemente queda "zombie"
(muerta a nivel TCP/WS pero sin que ninguna de las dos puntas lo note
todavía) durante el idle, y recién se destapa cuando llega un cambio de
contexto (play/transfer) - momento en el que Spotify "se da cuenta" y
fuerza el cierre. Y que quizás `go-librespot` se da cuenta *antes* de
esa situación, evitando el patrón.

**Confirmado leyendo `dealer/dealer.go` de `go-librespot`**: tiene
exactamente ese mecanismo, explícito. `pingTicker()` (línea 193) no solo
manda el ping JSON cada `pingInterval` (30s) - primero chequea
`timeSinceLastPong()` contra `pingInterval+timeout` (30+10=40s) y, si el
último pong está vencido, **cierra la conexión proactivamente**
(`d.closeConn(websocket.StatusServiceRestart)`) sin esperar a que el
servidor haga nada, dejando que el loop de reconexión normal levante una
conexión nueva:

```go
case <-ticker.C:
    timePassed := d.timeSinceLastPong()
    if timePassed > pingInterval+timeout {
        d.log.Errorf("did not receive last pong from dealer, %.0fs passed", timePassed.Seconds())
        // closing the connection should make the read on the "recvLoop" fail,
        // continue hoping for a new connection
        d.closeConn(websocket.StatusServiceRestart)
        continue
    }
```

`lastPong` se inicializa optimistamente a `now + pingInterval` al
conectar (`resetPongDeadline()`, dando margen al primer ping antes de
considerarlo vencido) y se actualiza a `time.Now()` cada vez que llega
un pong real (`case "pong":` en el receive loop).

Esto **encaja exactamente** con la única diferencia de comportamiento
observada hasta ahora entre ambos clientes (§40): el cierre en idle puro
de `go-librespot` no era un fenómeno aparte del de `cspot` - es
probablemente *el mismo* fenómeno (conexión que quedó zombie durante el
idle), sólo que `go-librespot` lo nota y actúa *antes* de que llegue
ningún comando, mientras que `cspot` - que nunca revisaba si sus propios
pings eran realmente contestados - se queda sentado sobre la conexión
muerta hasta que el próximo comando expone el problema (el servidor,
al ver actividad en una sesión que ya había dado por muerta/reciclable
del otro lado, fuerza el cierre en lugar de responder con normalidad).

**Confirmado leyendo `DealerClient.cpp` (`runTask()`, antes de este
fix)**: el JSON ping se mandaba cada `JSON_PING_INTERVAL_MS`
incondicionalmente (línea ~170), y la rama `"pong"` de `handleMessage()`
sólo hacía `CSPOT_LOG(debug, ...)` - **sin guardar ni comparar ningún
timestamp**. Si los pongs dejaban de llegar, no había ninguna
consecuencia: cspot seguía mandando pings al aire indefinidamente, sin
enterarse ni reconectar.

**Corregido** (`DealerClient.h`/`DealerClient.cpp`): agregado
`jsonPongDeadline` (miembro, no variable local - lo escribe tanto
`runTask()` como `handleMessage()`, aunque ambos corren en el mismo hilo
así que no hace falta mutex, ver el comentario del campo) y
`JSON_PONG_TIMEOUT_MS = 10000` (10s, igual que el `timeout` de
go-librespot), replicando el mecanismo exacto:

- Al conectar: `jsonPongDeadline = now + JSON_PING_INTERVAL_MS`
  (equivalente a `resetPongDeadline()`).
- En cada pong real recibido: `jsonPongDeadline = now` (equivalente a
  `lastPong = time.Now()`).
- En cada tick de ping (mismo punto donde antes solo se mandaba el
  ping): si `now - jsonPongDeadline >= JSON_PING_INTERVAL_MS +
  JSON_PONG_TIMEOUT_MS`, se loguea el error y se llama
  `transport->disconnect()` + `break` para salir del loop interno -
  cae naturalmente en el loop de reconexión existente de `runTask()`
  (mismo camino que cualquier otra desconexión detectada), sin tocar
  ese mecanismo.

**Verificado**: no compilable/testeable en host (`DealerClient.cpp`
depende de `WebSocketTransport`/`PlayerEngine`, no está en el
set de host tests actual, mismo motivo que TLSSocket.cpp - ver
`docs/host_tests.md`). Revisado a mano: `transport->disconnect()` existe
en la interfaz (`WebSocketTransport.h`), el `break` cae exactamente en
el mismo punto que el fin normal del loop (`connectionId.clear()` +
log de "reconectando" + vuelta al `while(running)` externo), y
`jsonPongDeadline` sólo lo tocan `runTask()` y `handleMessage()`, ambos
en el mismo hilo (confirmado en §35: el diseño de dos hilos para el
ping fue revertido, todo el Dealer corre en un solo hilo). Pendiente
prueba en hardware real: dejar el dispositivo en idle un buen rato sin
mandar ningún comando y ver si aparece el nuevo log "did not receive
last pong from dealer, forcing reconnect" *antes* de que llegue un
comando real - eso confirmaría la hipótesis del usuario y, si el
patrón de cierre post-comando desaparece con este fix, sería la causa
raíz real de toda esta investigación.

**Resultado**: probado en hardware - **sigue pasando**, sin que aparezca
el log del watchdog nuevo. Esto es en sí mismo un dato importante: si
el watchdog nunca dispara ni loguea nada, es porque el hilo del Dealer
nunca vuelve a llegar al punto del loop donde se revisa - está
atascado en otro lado. El usuario preguntó, acertadamente, si tendría
que ver con el manejo de `MBEDTLS_ERR_SSL_TIMEOUT` - ver §44b.

## 44b. El propio fix de §43 podía tragarse un timeout real y quedar atascado leyendo para siempre

**Repasando el loop agregado en §43**:

```cpp
do {
  ret = mbedtls_ssl_read(&ssl, buf, len);
} while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
         ssl.MBEDTLS_PRIVATE(state) == MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET);
```

La segunda mitad del `while` reintenta mientras `ssl.state` siga
marcando "procesando ticket", **sin importar qué valor tenga `ret`**.
Si un `NewSessionTicket` empieza a llegar pero nunca termina de
llegar - exactamente el escenario "zombie" que motivó toda esta
sección (§44): la conexión ya está muerta de verdad, y lo que se
alcanzó a recibir fue sólo el principio de un ticket - `ssl.state`
queda parado en `MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET` **para
siempre**, y este loop reintenta `mbedtls_ssl_read()` indefinidamente
en vez de devolver el control alguna vez.

Confirmado que `esp_mbedtls_read()` de ESP-IDF tiene el mismo hueco
exacto (misma condición de loop, sin ningún caso especial para
`MBEDTLS_ERR_SSL_TIMEOUT` dentro de él, ver la fuente completa citada
en §43) - pero `esp_http_client` no hace polling tan agresivo como
`EspWebSocketTransport::receiveMessage()`, que llama a
`TLSSocket::read()` (indirectamente, vía `readExact()`) más o menos una
vez por segundo durante toda la vida idle del Dealer (`RECEIVE_POLL_MS`
en `DealerClient.cpp`). Con ese patrón de uso, este hueco deja de ser
un caso de laboratorio.

**Por qué esto explica que el watchdog de §44 nunca disparara**: si el
hilo del Dealer queda atascado *dentro* de esta única llamada a
`TLSSocket::read()` (reintentando para siempre porque `ssl.state` nunca
se destraba), nunca vuelve al loop de `runTask()` que revisa el
watchdog de pongs perdidos - ese código sencillamente no se vuelve a
ejecutar. Encaja con lo observado: "sigue pasando" sin ningún log nuevo
del watchdog, ni siquiera durante el rato de idle previo al comando.

**Corregido** (`TLSSocket::read()`): un `MBEDTLS_ERR_SSL_TIMEOUT` real
ahora corta el loop incondicionalmente, sin importar el estado de
`ssl.state` - un timeout genuino nunca debe tragarse. Esto no cambia el
comportamiento en el caso normal (ticket que se termina de recibir
dentro del timeout, o datos de aplicación reales) - sólo evita quedar
atascado cuando el ticket nunca termina de llegar.

**Verificado**: mismo límite que §42/§43/§44 - no compilable/testeable
en host (`TLSSocket.cpp` es `#ifdef ESP_PLATFORM`). Pendiente hardware:
si esto era la causa real, con este fix puesto debería empezar a verse
el log del watchdog de §44 ("did not receive last pong...") disparando
él solo durante un idle largo sin comandos - la conexión debería
reconectarse *antes* de que llegue el próximo comando, en vez de que el
comando exponga un cierre. Si el patrón de cierre post-comando
desaparece con ambos fixes juntos (§44 + §44b), confirmaría de una vez
la hipótesis "conexión zombie" del usuario como causa raíz real.

## 45. Nueva inestabilidad tras §43/§44/§44b: el connect-state PUT se queda mudo - bug real (y viejo) en `SocketStream.cpp` finalmente confirmado

**Reportado por el usuario**: "mejoras aunque un poco inestable". Log
real de hardware pegado por el usuario: el dispositivo reproduce sin
problema (fetches de CDN cada ~2.5s, dos transiciones de pista con
`EOF`/`Playing done`/`consumeTrack`, Dealer WS mandando/recibiendo
ping/pong JSON cada 30s con total normalidad) durante más de un minuto
y medio - pero **ni un solo log de `connect-state PUT ok` ni `PUT
failed` aparece en todo el tramo**, pese a dos transiciones de pista
reales (que deberían disparar un PUT `PLAYER_STATE_CHANGED` cada una,
ver `PlayerEngine::runTask()`). Sintomáticamente: "en el cliente
no aparece ni que esté conectado ni reproduciendo" - la app de Spotify
nunca se entera, aunque el dispositivo esté reproduciendo audio real.

**Causa**: releyendo `SocketStream.cpp` (el mismo archivo ya señalado
como sospechoso en la investigación previa a §43, ver el resumen de
sesión anterior) con la pista de que "ni un log aparece" implica que
`sendPutStateRequest()` se queda **atascado antes** de llegar a
loguear éxito o error - es decir, atascado dentro de la escritura
HTTP, no de la lectura. `SocketBuffer::sync()` y `::xsputn()`
(`bell/main/io/SocketStream.cpp`) comparaban `bw < 0`, donde `bw` es
`ssize_t` pero viene de `TLSSocket::write()`, que **siempre** devuelve
un `size_t` recortado a 0 en cualquier error de mbedTLS (nunca
negativo - ver el comentario de F30 en `TLSSocket.cpp`). Con eso,
`bw < 0` **nunca es cierto**, así que una escritura que realmente
falla (conexión muerta) se ve idéntica a "escribí 0 bytes de una vez
más" - y como `n -= bw` (con `bw == 0`) nunca reduce `n`, el
`while (n > 0)` de `sync()` reintenta `internalSocket->write()` **para
siempre**, sin salir jamás con el `return -1` que debería.

Encaja perfectamente: el connect-state PUT usa una conexión
`putConnection` deliberadamente *reutilizada/keep-alive* entre llamadas
(`PlayerEngine.cpp`, comentario en `sendPutStateRequest()`) -
exactamente el tipo de conexión que puede quedar inactiva (los PUTs
están limitados a como mucho uno cada `PUT_MIN_INTERVAL_MS`) el tiempo
suficiente para que el peer la dé de baja del otro lado. Cuando
`flush()` intenta escribir el siguiente PUT sobre esa conexión ya
muerta, cae en este loop infinito - el hilo de `PlayerEngine`
queda congelado ahí *para siempre*, nunca llega al `try`/log de
`sendPutStateRequest()`, y ningún PUT posterior se vuelve a intentar
tampoco (es un solo hilo, con su propio loop de `pendingCv.wait_for`
que nunca vuelve a correr). Mientras tanto la reproducción de audio
sigue perfecta porque usa conexiones completamente separadas
(Mercury/CDN, abiertas frescas cada vez, no reutilizadas idle) y el
Dealer WS sigue mandando su ping JSON con normalidad (otro hilo, otra
conexión) - de ahí la separación exacta observada en el log entre "todo
bien" (reproducción, Dealer) y "silencio total" (PUT).

Este bug **no es nuevo de esta sesión** - ya había quedado señalado
(sin corregir) en el resumen de la sesión anterior a raíz de la
investigación de `HTTPClient`/`TLSSocket`/`SocketStream` de §42/§43,
pero en ese momento se consideró "probablemente causa un hang, no la
excepción reportada" y quedó pendiente. Los fixes de §43/§44/§44b
probablemente no lo *causaron* - lo más probable es que ya estuviera
ahí desde siempre y recién ahora, con sesiones más largas e
interacciones más variadas gracias a que TLS 1.3 finalmente bootea
(§43), hay suficiente tiempo de conexión keep-alive inactiva como para
que el peer la cierre y se dispare.

**Corregido**: cambiado `bw < 0` a `bw <= 0` en ambos lugares (`sync()`
y `xsputn()`), igualando el patrón ya correcto que `underflow()`/
`xsgetn()` ya usaban (`br <= 0`). Una escritura de 0 bytes sobre un
pedido de más de 0 bytes siempre es una falla real en este código (no
hay ningún caso donde escribir 0 sea éxito parcial legítimo) - el `<=`
cierra el hueco sin cambiar ningún comportamiento del camino feliz.

**Verificado**: `unit_tests` 27/27 (112 asserts) y `f93_concurrency_test`
("completed without a detected data race") corridos limpios contra el
build real de host tras el cambio - `SocketStream.cpp` sí se compila y
ejercita ahí (a diferencia de `TLSSocket.cpp`/`DealerClient.cpp`),
así que esta es la primera corrección de esta sesión con verificación
real de host, no sólo revisión manual. Pendiente hardware: confirmar
que los PUTs de connect-state vuelven a aparecer en el log
("connect-state PUT ok...") incluso después de idle largo, y que el
cliente de Spotify vuelve a mostrar el dispositivo como conectado/
reproduciendo.

## 46. Reproducido en hardware con todos los fixes puestos - y confirmado: el cierre en sí ya no es un problema

**Log real** (usuario, hardware con §43+§44+§44b+§45 aplicados): tras
más de dos minutos reproduciendo con normalidad, llega un comando real
(`play`), se procesa con éxito
(`DealerClient.cpp:438: ... endpoint=play -> success=1`), se manda la
respuesta, y ~18ms después de mandar el siguiente ping JSON de rutina
aparece **el mismo cierre de siempre**:
`EspWebSocketTransport.cpp:610: Dealer WebSocket: peer sent WebSocket
close, disconnecting` - exactamente el patrón que motivó toda esta
investigación desde el principio, sin cambios. El watchdog de §44
**no** disparó antes de esto (no hay log de "did not receive last
pong"), confirmando que la conexión no estaba "zombie" en el sentido
que se sospechaba - los pongs habían estado llegando con normalidad.

**Pero la diferencia real está en lo que pasa después**, y es
sustancial:

```
[00:14:38.861] Dealer: connection lost, reconnecting
[00:14:40.971] Dealer: connected to guc3-dealer.spotify.com:443
[00:14:40.978] Dealer: got Spotify-Connection-Id (200 chars)
[00:14:41.007] connect-state PUT ok (reason 4)
[00:14:41.276] connect-state PUT ok (reason 3)
```

Reconexión limpia en ~2s, nueva connection-id, y los PUTs de
connect-state **funcionan de inmediato** tras el reconnect - algo que
antes de §45 (el fix del hang en `SocketStream::sync()`/`xsputn()`)
directamente se colgaba para siempre. La reproducción de audio ni
se entera (los fetches de CDN de la pista en curso siguen fluyendo sin
ningún corte visible en el log). Confirmado directamente con el
usuario: **la app de Spotify muestra el dispositivo conectado y
reproduciendo con normalidad** después de este evento - "se recupera
bien".

**Conclusión de toda esta investigación (§20 a esta sección)**: el
cierre del Dealer WS en sí **nunca fue el bug** - es, con altísima
probabilidad, un reciclado de conexión completamente normal del lado
de Spotify (la misma clase de evento que `go-librespot` también sufrió
una vez, en idle puro, §40 - el server recicla conexiones Dealer
periódicamente, y como coincide con la primera interacción real tras
un buen rato de silencio, siempre pareció "disparado por el comando"
sin serlo realmente). Lo que sí eran bugs reales, y por eso hacían que
este evento normal se sintiera como una falla del dispositivo:

1. **TLS 1.3 nunca lograba completar el bootstrap de sesión** (§42/§43)
  - el `NewSessionTicket` post-handshake de mbedTLS 4.0 no se manejaba,
  cortando la sesión antes incluso de llegar al Dealer.
2. **El manejo de ese mismo `NewSessionTicket` podía quedarse atascado
  leyendo para siempre** si el ticket empezaba a llegar pero la
  conexión moría a mitad de camino (§44b) - congelando el hilo del
  Dealer.
3. **El connect-state PUT podía colgarse para siempre en un loop
  infinito de escritura** sobre una conexión keep-alive ya muerta
  (§45, el bug real y más importante de los tres) - así que aunque el
  Dealer reconectara bien, el dispositivo nunca lograba avisarle a
  Spotify que estaba vivo y reproduciendo. Este es, con la evidencia de
  esta sección, **el bug que realmente explicaba el síntoma original**
  ("el cliente no muestra conectado/reproduciendo tras un rato de
  idle + un comando") - no el cierre del WS en sí, que es inevitable y
  normal, sino la incapacidad de recuperarse de él limpiamente.

De yapa, el watchdog de pongs perdidos (§44) queda como una protección
razonable para el caso genuinamente "zombie" (conexión muerta sin que
el servidor la cierre nunca) aunque no haya sido ese el escenario que
se reprodujo acá - no hace daño dejarlo.

**Pendiente** (ya no bloqueante): seguir observando en uso normal
durante más tiempo para confirmar que no queda ningún otro caso borde
(reconexión bajo pérdida real de red, token expirado a mitad de
reconexión, etc.), pero el síntoma que abrió esta investigación se da
por resuelto.

## 47. Mensajes de log del Dealer alineados textualmente con `go-librespot` (2026-07-16)

Pedido explícito del usuario: quiere poder comparar logs de `cspot`
contra `go-librespot` lado a lado (como se viene haciendo durante toda
esta investigación) sin tener que traducir mentalmente frases
distintas para el mismo evento. Revisado `dealer/dealer.go` completo
(todas las llamadas a `d.log.*`) y alineado `DealerClient.cpp` a la
misma redacción exacta donde hay un evento equivalente real:

- `"sent dealer ping"` (antes "sent JSON ping").
- `"received dealer pong"` (antes "received JSON pong").
- `"did not receive last pong from dealer, %.0fs passed"` (antes "did
  not receive last pong, forcing reconnect (%lds overdue)" del propio
  §44) - texto igualado, unidad igual (segundos).
- `"failed sending dealer ping"` - **faltaba por completo**: antes, un
  `sendText()` de ping fallido se ignoraba en silencio (el `if` sólo
  logueaba el caso de éxito). go-librespot sí lo loguea (`Warnf`,
  `pingTicker()`). Agregado - `error` en vez de `warn` porque
  `Logger.h`/`CSPOT_LOG` de este proyecto sólo tiene
  `debug`/`info`/`error`, no hay nivel `warn`.
- `"starting dealer recv loop"` / `"re-established dealer connection"`
  - go-librespot desdobla esto en dos líneas mutuamente excluyentes:
  la primera conexión exitosa de toda la vida del `Dealer` loguea la
  primera (`startReceiving()`, una sola vez por `recvLoopOnce.Do`),
  cualquier reconexión posterior loguea la segunda (`reconnect()`).
  `DealerClient::runTask()` no distinguía esto (mismo camino de
  `connectOnce()` para ambos casos) - agregado un `bool everConnected`
  local a `runTask()` para replicar la misma distinción.
- `"connected to %s"` ya coincidía textualmente de antes, sin cambios.

**Deliberadamente NO tocado**: los mensajes de fallo de
`EspWebSocketTransport.cpp` (`describeReadFailure()`, "frame receive
stalled", "peer sent WebSocket close", etc.) son más específicos que el
genérico `"failed receiving dealer message"` de go-librespot - esa
granularidad fue clave para diagnosticar varios de los bugs de esta
misma investigación (§42-45), así que homogeneizarlos habría sido un
paso atrás en capacidad de diagnóstico a cambio de una comparación más
prolija. Iguales, en cambio, los eventos de rutina que sí se comparan
constantemente entre ambos clientes (ping/pong, reconexión) - que es lo
que motivó el pedido.

**Verificado**: `unit_tests`/`f93_concurrency_test`/
`f87_logger_concurrency_test` recompilados sin cambios (ninguno
depende de `DealerClient.cpp`, no está en el set de host tests -
mismo límite que siempre, ver §44). Revisado a mano: `CSPOT_LOG` no
tiene nivel `warn` (confirmado en `Logger.h`), de ahí el uso de
`error` para "failed sending dealer ping". Pendiente hardware: no
cambia ningún comportamiento, sólo texto de log - no hay nada que
"funcione distinto" que verificar, sólo confirmar que los mensajes
aparecen como se espera en la próxima sesión.

## 48. Auditoría de las clases de conexión de `bell` (`TCPSocket`, `SocketStream`, `HTTPClient`, `URLParser`) - 8 inconsistencias reales encontradas y corregidas (2026-07-16)

Pedido explícito del usuario: revisar las clases de `bell` asociadas a
conexión buscando inconsistencias, en el mismo espíritu que los bugs
reales ya encontrados esta sesión en `TLSSocket.cpp`/`SocketStream.cpp`
(§43-45). Revisados completos: `BellSocket.h`, `TCPSocket.h`,
`TLSSocket.h`, `SocketStream.h`/`.cpp`, `HTTPClient.h`/`.cpp`,
`URLParser.h`/`.cpp`. Ninguno de los hallazgos se dispara hoy en
producción (todo cspot usa `https://`, así que `TCPSocket` sólo corre
en los host tests), pero son inconsistencias reales entre las dos
implementaciones de `bell::Socket` y en `HTTPClient`.

**`TCPSocket.h`** (contraparte no-TLS de `TLSSocket`, usada por los
host tests vía `http://`):

1. **`read()`/`write()` sin clamp de negativos** - devolvían
  `recv()`/`send()` (pueden ser `-1`) convertidos directo a `size_t`,
  sin el clamp explícito que `TLSSocket` sí tiene (documentado como
  F30/§45: cualquier error negativo se vuelve `0`). Antes de este fix,
  "funcionaba" sólo porque el patrón de bits de `-1` sobrevive el viaje
  `size_t`→`ssize_t` en `SocketStream.cpp` en este toolchain - exactamente
  la situación que el propio comentario de F30 en `TLSSocket.cpp` señala
  como "no garantizada". Corregido: `ssize_t ret = recv(...); return ret
  < 0 ? 0 : (size_t)ret;` (igual en `write()`).
2. **Fuga de `addrinfo*` en `open()`** - si `connect()` fallaba, la
  función hacía `throw` antes de llegar al `freeaddrinfo(addr)` de más
  abajo. Corregido: `freeaddrinfo(addr)` agregado también en esa rama,
  antes del `throw`.
3. **Sin timeout de lectura/escritura** - a diferencia de `TLSSocket`
  (`readTimeoutMs`/`SO_SNDTIMEO`, F58/§22), `TCPSocket` no configuraba
  ningún timeout - un peer colgado bloquearía `recv()`/`send()` para
  siempre. Corregido: `SO_RCVTIMEO`/`SO_SNDTIMEO` de 15000ms (mismo
  default que `TLSSocket`) agregados en `open()`, con rama separada para
  Windows (`SO_RCVTIMEO`/`SO_SNDTIMEO` esperan un `DWORD` de
  milisegundos ahí, no un `struct timeval` como en POSIX - ABI distinta,
  no sólo un cast).
4. **`sockFd` sin inicializar** - `int sockFd;` sin valor por defecto.
  `getFd()` antes de `open()` devolvía basura, no un centinela seguro
  como `-1` (a diferencia de `TLSSocket`, donde `mbedtls_net_init()` en
  el constructor deja `server_fd` en un estado definido desde el
  principio, no recién después de `open()`). Relevante porque
  `SocketStream::getFd()` existe justamente para que un dueño con un
  hilo lector bloqueado pueda desbloquearlo vía `::shutdown()` sobre ese
  fd - operar sobre basura podría apuntar a un fd ajeno no relacionado.
  Corregido: `int sockFd = -1;`.

**`SocketStream.h`**:

5. **El constructor de 3 argumentos de `SocketBuffer` ignoraba
  `isSSL`** - `SocketBuffer(hostname, port, isSSL)` llamaba a
  `open(hostname, port)` (2 args), así que `isSSL` nunca se usaba,
  siempre abría TCP plano sin importar lo que se pasara. Sin
  llamadores hoy (código muerto, confirmado por grep), pero una trampa
  real para el próximo que lo use esperando TLS. Corregido: pasa
  `isSSL` a través.

**`HTTPClient.h`/`.cpp`**:

6. **Orden de parámetros de `rawRequest()` no coincidía entre `.h` y
  `.cpp`** - el header declaraba `(method, url, ...)`, la
  implementación real era `(url, method, ...)`. Compilaba y linkeaba
  bien porque ambos son `std::string` (incompatibilidad de tipos que el
  compilador jamás habría dejado pasar en silencio si los tipos
  difirieran) y los 3 únicos llamadores reales (`get`/`post`/`put`,
  todos en el mismo archivo) ya usaban el orden real del `.cpp` - pero
  el header público mentía sobre el orden, listo para intercambiar
  silenciosamente método↔url en cualquier llamador nuevo que confiara
  en él. Corregido: el header ahora declara `(url, method, ...)`,
  igual que la definición real.
7. **Sin soporte de `Transfer-Encoding: chunked`** - `readRawBody()`
  sólo leía si había `Content-Length`; una respuesta chunked (sin ese
  header) quedaba con body vacío, sin ningún error - una falla
  silenciosa real para cualquier endpoint que alguna vez responda así.
  Corregido: `readResponseHeaders()` ahora detecta `Transfer-Encoding:
  chunked` (case-insensitive), y `readRawBody()` bifurca a un nuevo
  `readChunkedBody()` que implementa RFC 7230 §4.1 completo (línea de
  tamaño hex + `;extensión` opcional ignorada, datos del chunk +
  CRLF final, chunk de tamaño 0 termina, trailers opcionales
  descartados hasta la línea vacía final). Se agregó también
  `bool bodyRead` para no releer el body en una segunda llamada a
  `body()`/`bytes()` sobre el mismo `Response` (el guard viejo,
  `contentSize > 0 && rawBody.size() == 0`, no cubría un body chunked
  legítimamente vacío ni distinguía "todavía no leído" de "ya leído y
  vacío").
8. **`std::stoi()` sin capturar sobre `Content-Length`/`content-range`**
  - un valor malformado del peer lanzaba `std::invalid_argument`/
  `out_of_range` sin capturar, no `std::runtime_error` - igual lo
  atrapan los `catch (const std::exception&)` externos de cada
  llamador real, pero se saltaba la lógica de "reintentar una vez" de
  `rawRequest()` (que sólo atrapa `std::runtime_error`) y reportaba un
  tipo de error distinto/más críptico para lo que en el fondo es el
  mismo tipo de problema ("respuesta del peer malformada"). Corregido:
  ambos sitios ahora envuelven el `stoi()` en un `try`/`catch` propio
  que relanza como `std::runtime_error` con un mensaje claro.

**Deliberadamente no tocado**: `URLParser.cpp`/`.h` - revisado a fondo
(incluyendo el chequeo de `match.size() < 3`, que en un primer análisis
pareció no cubrir el caso de `regex_match()` devolviendo `false` sin
chequear su valor de retorno - pero `std::match_results::size()` es
`0` precisamente cuando el match global falló, así que ese chequeo ya
cubre ese caso correctamente, no hay bug ahí). También se revisó con
cuidado el `memcpy` de restauración de `\r\n` en
`HTTPClient::readResponseHeaders()` (pareció, en un primer paso,
corromper el último byte de cada línea) - descartado tras verificar que
`gcount()` de `getline()` sí cuenta el delimitador consumido-pero-no-
almacenado, así que el offset del `memcpy` cae exactamente donde
corresponde, sin corrupción real.

**Verificado**:
- `unit_tests`: 27/27 casos, 112/112 asserts (ejercita `TCPSocket` real
  vía `http://` en `fake_cdn_server` y el `HTTPClient` real - a
  diferencia de `TLSSocket.cpp`/`DealerClient.cpp`, estas clases SÍ son
  100% host-testables, confirmado corriendo la suite completa, no sólo
  revisión manual).
- `f93_concurrency_test`: sin data race detectado.
- `f87_logger_concurrency_test`: 1200 líneas concurrentes, sin
  interleaving.
- **Chunked encoding, verificado aparte con un programa standalone**
  (no parte de la suite oficial, en el scratchpad de la sesión):
  compilado `HTTPClient.cpp`/`SocketStream.cpp`/`URLParser.cpp`/
  `picohttpparser.c`/`BellLogger.cpp` + `TLSSocketStub.cpp` (mismo
  patrón que `unit_tests`) contra un servidor Python armado a mano que
  manda una respuesta HTTP real de 3 chunks (`Transfer-Encoding:
  chunked`, sin `Content-Length`). El body reensamblado por
  `HTTPClient::body()` coincidió byte a byte con el original - no
  quedó como una implementación sin probar.

Todos los hallazgos son inconsistencias reales y confirmadas, no
teóricas - pero **latentes** en este proyecto específico (ningún URL de
producción usa `http://` ni depende de una respuesta chunked de
Spotify/CDN hoy). Se corrigieron de todas formas porque son el mismo
tipo de bug (asimetría entre las dos implementaciones de `bell::Socket`,
manejo de errores inconsistente) que ya causó problemas reales esta
sesión (§45), y porque `bell` es una capa compartida - un futuro uso de
`http://` o una respuesta chunked ya no encontraría estas trampas.

## 49. `cspot` nunca activaba TCP keepalive en ningún socket - `go-librespot` lo tiene desde siempre vía el dialer default de Go (2026-07-16)

**Pregunta del usuario**: ¿podrá `go-librespot` detectar una conexión
WebSocket zombie antes que nosotros? ¿hay algo que tardemos en notar?

**Investigado**: repasados TODOS los mecanismos periódicos/keepalive de
ambos clientes para buscar una asimetría no vista todavía:
- `daemon/player.go:803` (`flushState`) - descartado como teoría
  aparte (mensaje anterior): es un coalescer de eventos reales, no un
  heartbeat.
- Ping/pong JSON del Dealer (30s+10s) - ya replicado (§44).
- Ping/pong/pongAck a nivel AP (`ap/ap.go`, `pongAckInterval=120s`) -
  ya tiene equivalente real en `MercurySession.cpp`
  (`PING_TIMEOUT_MS=125s`, estructurado distinto -  el servidor manda
  el ping y nosotros revisamos que siga llegando, en vez de que
  nosotros mandemos el pong y el servidor lo confirme - pero mismo
  orden de magnitud, no es un hueco real).

**Lo encontrado, sí es un hueco real**: el WebSocket del Dealer de
`go-librespot` se conecta vía `websocket.Dial()` usando el
`*http.Client` que le pasa `session.go` (`&http.Client{Timeout: 30 *
time.Second}`, sin `Transport` propio) - lo que significa que usa
`http.DefaultTransport` de Go, cuyo dialer trae, desde siempre,
`KeepAlive: 30 * time.Second`. Es decir: **la conexión Dealer de
go-librespot tiene TCP keepalive a nivel de sistema operativo cada
30s**, sondeando la conexión de forma completamente independiente del
ping/pong JSON a nivel aplicación - nadie lo configuró a propósito,
viene gratis con el transport HTTP default de Go.

`cspot` **nunca activó `SO_KEEPALIVE` en ningún socket** - confirmado
por grep sobre `TLSSocket.cpp`, `TCPSocket.h` y `PlainConnection.cpp`:
cero ocurrencias. La feature SÍ está compilada en el lwIP de ESP-IDF
(`LWIP_TCP_KEEPALIVE=1` en `components/lwip/port/include/lwipopts.h`,
confirmado leyendo el archivo real, no asumido) y soporta el socket API
completo (`SO_KEEPALIVE`, `TCP_KEEPIDLE`, `TCP_KEEPINTVL`,
`TCP_KEEPCNT` - confirmados en `lwip/src/include/lwip/sockets.h`) -
simplemente nunca se usó.

**Por qué importa**: un keepalive a nivel TCP corre en el kernel/stack
de red, no en la aplicación - detecta un peer verdaderamente muerto
(un NAT que tiró el mapeo, un middlebox que dejó de responder) de forma
independiente de cualquier bug o mal funcionamiento a nivel aplicación
(el propio watchdog de pongs de §44 depende de que el hilo del Dealer
siga corriendo con normalidad - si algo lo bloquea, como el bug real de
§44b que sí llegamos a reproducir, ese watchdog tampoco corre). El
keepalive de TCP es una capa de detección extra, totalmente
independiente, que `go-librespot` tiene desde siempre sin haberlo
buscado a propósito y `cspot` nunca tuvo en absoluto.

**Corregido**: `TLSSocket::open()` (afecta a TODO uso de `TLSSocket` -
Dealer WS, PUT de connect-state, Login5, CDN, context-resolve) y
`PlainConnection.cpp` (socket AP/Mercury) ahora activan
`SO_KEEPALIVE=1` + `TCP_KEEPIDLE=30` + `TCP_KEEPINTVL=10` +
`TCP_KEEPCNT=3` (peor caso ~60s para detectar un peer muerto - al menos
tan agresivo como el default de 30s de Go, con margen). En
`PlainConnection.cpp`, el tuning fino (`TCP_KEEPIDLE`/`INTVL`/`CNT`)
queda detrás de `#ifndef _WIN32` - Windows necesita
`WSAIoctl(SIO_KEEPALIVE_VALS)` para eso, una API distinta a
`setsockopt()` - pero el `SO_KEEPALIVE` simple sí se activa en ambos,
usando los timers default del SO en Windows.

**Verificado**: `PlainConnection.cpp` es host-testable (a diferencia de
`TLSSocket.cpp`, `#ifdef ESP_PLATFORM`) - `unit_tests` 27/27 (112
asserts), `f93_concurrency_test` sin data race, `f87_logger_concurrency_
test` sin interleaving, los tres recompilados limpios con el cambio.
`TLSSocket.cpp` sólo revisado a mano (mismo límite de siempre).
Pendiente hardware: no cambia ningún comportamiento observable
directamente (el keepalive es invisible en los logs salvo que
efectivamente detecte algo) - lo relevante a confirmar es si, con esto,
alguna vez se ve una reconexión de Dealer/PUT que NO coincida con un
comando real (es decir, el propio keepalive detectando y forzando la
baja de una conexión zombie antes de que el usuario haga nada) - eso
confirmaría que esta capa efectivamente está haciendo algo que antes no
pasaba.

**Confirmado en hardware real - "eureka, era eso" (usuario)**: probado
tras un buen rato de idle seguido de un comando real - **la conexión ya
no se cierra**. Este es el resultado que toda la investigación (§20 en
adelante) venía persiguiendo.

**Esto revisa la conclusión de §46**: en ese momento, con la evidencia
disponible (reconexión siempre limpia, sin ningún síntoma visible del
lado del usuario), se concluyó que el cierre era muy probablemente un
reciclado de conexión normal del lado de Spotify - algo inevitable,
que como mucho se podía manejar con gracia. La evidencia de esta
sección apunta a algo distinto y más preciso: si el keepalive de TCP
por sí solo **evita que el cierre ocurra**, es mucho más consistente
con un **camino de red silenciosamente semi-muerto** (un NAT o
tracker de conexión de un operador que expira su entrada por falta de
tráfico real a nivel TCP durante el idle) que el servidor termina
notando y reaccionando cerrando la sesión - no una decisión activa de
"reciclar" por parte de Spotify. El keepalive de TCP genera
exactamente el tipo de tráfico de bajo nivel que mantiene ese camino
vivo, evitando que llegue a expirar en primer lugar. Esto también
explica limpiamente por qué `go-librespot` nunca mostró este patrón:
tenía esta protección desde siempre, sin que nadie la pidiera a
propósito, sólo por venir de serie en el transport HTTP default de Go
(§49, más arriba).

Con esto, la cadena completa de la investigación (§20-§49) queda
resuelta: el síntoma original - cierre del Dealer WS tras idle seguido
de un comando, con el dispositivo apareciendo desconectado para el
usuario - tenía tres causas reales y acumulativas, todas corregidas:
TLS 1.3 nunca lograba completar el bootstrap (§42/§43), el manejo de
ese mismo mecanismo podía quedar atascado leyendo para siempre (§44b),
el PUT de connect-state podía colgarse para siempre en un loop de
escritura sobre una conexión muerta (§45) - y, la causa raíz real del
cierre en sí, la ausencia total de TCP keepalive dejando que el camino
de red se degradara silenciosamente durante el idle (§49).

## 50. Retirados los reconnects proactivos basados en idle-time - confirmado que `go-librespot` no tiene equivalente para PUT/CDN (2026-07-16)

**Motivación del usuario**: con el keepalive de TCP (§49) confirmado
como la causa raíz real (el cierre del Dealer ya no ocurre en
absoluto), varios de los mecanismos de "reconexión proactiva" agregados
antes de encontrar esa causa raíz pasan a ser parches para un síntoma
que ya no debería producirse. Antes de tocar nada, se preguntó
explícitamente si `go-librespot` tiene algo equivalente a estos
mecanismos - **confirmado que no**, revisando el código real:

- `daemon/player_state.go`/`player.go` (PUT de estado, equivalente al
  `spclient` de `PlayerEngine`): usa el `*http.Client` compartido
  (`&http.Client{Timeout: 30s}`, sin `Transport` propio) - depende
  enteramente de `http.DefaultTransport`, cuyo pool de conexiones
  reutilizables expira solo (`IdleConnTimeout`, 90s por default) y
  reintenta automáticamente una vez si la conexión reusada resultó
  estar muerta - comportamiento genérico de la librería estándar de Go,
  no código custom escrito por `go-librespot`. No existe ningún
  `stateDirty`/timer relacionado con "edad" de la conexión en sí (sólo
  el coalescing de `updateState()`, ver mensaje anterior sobre
  `flushState`).
- No se encontró nada equivalente para el fetch de audio/CDN tampoco
  (grep de `stale`/`lastUsed`/`IdleConnTimeout` en todo el repo no
  encontró código relacionado, sólo coincidencias de la palabra "stale"
  en comentarios sin relación).
- Lo único que `go-librespot` sí tiene escrito a mano es el watchdog de
  pongs perdidos del Dealer (`dealer.go`, ya replicado en `cspot` como
  §44) - exactamente el punto 1 de los tres que se evaluaron, y el
  único que se mantiene.

**Retirado** (puntos 2 y 3, ambos agregados esta sesión o preexistentes
pero motivados por la misma teoría de "conexión reusada que puede
quedar vieja"):

- `PlayerEngine.cpp`/`.h`: `STALE_PUT_CONNECTION_THRESHOLD_S`
  (45s) y su chequeo en `sendPutStateRequest()` y `putStateInactive()`
  (ambos tenían una copia idéntica del mismo bloque), más el miembro
  `lastPutConnectionTime` que sólo servía para esto. Ambas funciones
  vuelven al patrón simple: reusar `putConnection` si existe, dejar que
  el `catch` externo (`putConnection.reset(); spclientHost.clear();`)
  maneje una reconexión reactiva si la reutilización tira una
  excepción - el mismo patrón F82/F58 que `HTTPClient::rawRequest()` ya
  tiene internamente.
- `CDNAudioFile.h`/`.cpp`: `STALE_CONNECTION_THRESHOLD` (20s, esta sí
  preexistente, de antes de esta sesión) y el chequeo `likelyStale` en
  `fetchRange()`, más el campo `CDNConnection::lastUsed` que sólo
  servía para esto. `fetchRange()` vuelve a depender exclusivamente de:
  mismo host → intentar reusar, `catch` → reconectar fresco (mismo
  patrón reactivo de siempre, nunca se tocó esa parte).

**No tocado**: el watchdog de pongs perdidos de `DealerClient.cpp`
(§44) - es el único de los tres con un equivalente real y confirmado
en `go-librespot`, se mantiene tal cual.

**Verificado**: `grep` completo del árbol confirma cero referencias
sobrantes a los símbolos retirados (`STALE_PUT_CONNECTION_THRESHOLD_S`,
`lastPutConnectionTime`, `STALE_CONNECTION_THRESHOLD`,
`CDNConnection::lastUsed`). `unit_tests` 27/27 (112 asserts) -
`CDNAudioFile.cpp` sí se recompiló y corrió con el cambio (host-
testable). `PlayerEngine.cpp` no está en el set de host tests
(mismo límite de siempre, depende de `TLSSocket`/`DealerClient`) -
revisado a mano, ambas funciones quedan sintácticamente limpias y
simétricas entre sí. `f93_concurrency_test`/`f87_logger_concurrency_
test` sin regresión. `<chrono>` movido de `CDNAudioFile.h` (ya no lo
necesita) a `CDNAudioFile.cpp` (sí lo sigue necesitando, para medir
cuánto tarda cada fetch - uso no relacionado con lo retirado, que antes
dependía del include transitivo del header).

**Pendiente hardware**: confirmar que, sin estos dos mecanismos, tanto
el PUT de connect-state como el fetch de CDN se siguen recuperando con
normalidad de una conexión reusada-pero-muerta (vía el camino reactivo
que siempre existió) ahora que el keepalive de TCP (§49) debería evitar
que esa situación ocurra en primer lugar.

## 51. Revisados los commits del PR #48 de `bell` upstream (`feelfreelinux/bell`) - 2 hallazgos más aplicados

Pedido del usuario: revisar los 18 commits de
[feelfreelinux/bell#48](https://github.com/feelfreelinux/bell/pull/48)
(el mismo PR de donde salió el intento de `SO_LINGER` en §23) buscando
algo más aprovechable. La mayoría son ajenos a esta rama (CivetWeb,
sink de audio VS1053, CI, prioridad de pthreads en ESP32). Dos SÍ son
directamente aplicables - el propio PR upstream identificó, de forma
independiente, la misma familia de problemas que esta sesión viene
corrigiendo ("garbage data in buffers due to mbedtls mismanagement" -
mismo espíritu que F30/§45/§48):

- **Commit `89d833e`** ("Enhance error handling for mbedtls in
  ESP32"): entre otras cosas, agrega chequeos de `internalSocket ==
  nullptr` al principio de `sync()`/`underflow()`/`xsgetn()` en
  `SocketStream.cpp`, lanzando una excepción clara en vez de permitir
  un null-pointer-dereference (crash duro, no atrapable) si alguna de
  esas funciones se llama sobre un stream nunca abierto o ya cerrado.
  **Aplicado tal cual** - `SocketStream.cpp` no tenía este chequeo en
  ninguna de las tres funciones. `xsputn()`/`overflow()` quedan
  cubiertas transitivamente (ambas llaman a `sync()` antes de tocar
  `internalSocket` directamente).
- **Commit `d755889`** ("Add out-of-bounds check for header name and
  value in readResponseHeaders()"): `phr_parse_response()` devuelve
  punteros `name`/`value` que apuntan **dentro** de `httpBuffer` -  se
  usaban sin validar. **Aplicado tal cual** (con el chequeo del límite
  inferior agregado además del superior que traía el commit original):
  si algún día la contabilidad de `httpBufferAvailable`/`prevbuflen`
  sobre el buffer reusado (conexiones keep-alive) tuviera un bug
  propio, esto lo convierte en una excepción clara en vez de una
  lectura fuera de rango silenciosa.

**Descartado, no directamente aplicable**: el mismo commit `89d833e`
también agrega un retry con límite de 10 intentos + delay de 10ms en
`TLSSocket::read()`/`write()` para `MBEDTLS_ERR_SSL_WANT_READ` y un
código -34 ("network temporarily unavailable") - pero esa técnica
asume una configuración de I/O no-bloqueante. `TLSSocket` de este
proyecto usa `mbedtls_net_recv_timeout` (bloqueante con timeout, ver
`open()`), donde `WANT_READ`/`WANT_WRITE` no deberían aparecer nunca en
la práctica (la función de recepción subyacente ya bloquea/espera en
vez de devolver "reintentá" - confirmado razonando sobre el mismo
código en §43/§44b) - adoptar ese retry específico sin también cambiar
el modelo de I/O no aporta nada acá.

**Verificado**: `unit_tests` 27/27 (112 asserts), `f93_concurrency_test`
sin data race, `f87_logger_concurrency_test` sin interleaving, los tres
recompilados limpios. Reverificado además el test standalone de
chunked encoding (§48) contra el `HTTPClient.cpp` actualizado - sigue
reensamblando el body byte a byte correctamente.

## 52. Auditoría de `cspot_connect.cpp` buscando código que debería vivir en `bell` - 2 hallazgos reales, aplicados

Pedido del usuario: revisar `cspot_connect.cpp` buscando código que
debería estar en `bell` (genérico) o en `cspot` en vez de repetido en
la capa de integración de la app, y si el uso directo de
`esp_http_server`/`mdns` en vez de las abstracciones de `bell` es un
problema.

**`esp_http_server` directo - no es un problema, es la decisión
correcta**: `bell::BellHTTPServer` (sobre CivetWeb) existe, pero
`components/cspot/CMakeLists.txt` define
`set(BELL_DISABLE_WEBSERVER ON)` - CivetWeb ni siquiera se compila para
el build ESP32 de este proyecto. Usar `esp_http_server` (el componente
nativo de ESP-IDF) directo es deliberado, no un descuido.

**mDNS - hallazgo real, corregido**: `bell::MDNSService` tiene 4
implementaciones por plataforma (Linux/Apple/Win32/ESP). La de Linux
arranca sola el responder mDNS por dentro de `registerService()`
(`avahi_client_new()`/`mdnsd_start()` en el primer llamado, con
`gethostname()` para el hostname) - quien la llama nunca toca la API
de mDNS cruda. La de ESP **no** hacía esto - `registerService()` sólo
llamaba a `mdns_service_add()`, y exigía que quien llama ya hubiera
corrido `mdns_init()`/`mdns_hostname_set()`/`mdns_instance_name_set()`
por su cuenta - exactamente lo que `cspot_connect.cpp` hacía a mano en
`startHttpServerAndMdns()`. Corregido: `MDNSService.cpp` (ESP) ahora
arranca el responder solo, de forma perezosa (primera llamada), usando
`serviceName` (el mismo valor que ya se pasaba a mano a
`mdns_hostname_set()`/`mdns_instance_name_set()`) - no hizo falta
cambiar la firma de `registerService()`. `cspot_connect.cpp` ya no
llama a la API de mDNS cruda en absoluto (se le quitó el `#include
"mdns.h"`, ahora innecesario).

**`urlDecode()` duplicado - hallazgo real, corregido**:
`cspot_connect.cpp` tenía su propia copia de
`bell::URLParser::urlDecode()`, casi idéntica - con una diferencia real:
la copia local sí validaba que los dos caracteres después de `%` fueran
dígitos hexadecimales antes de decodificar; la de `bell` llamaba
`strtol()` a ciegas sobre lo que siguiera, produciendo basura en
silencio ante un escape malformado/truncado (ej. `%zz` decodificaba a
un byte NUL inyectado, en vez de dejar el texto literal). Corregido:
la validación se llevó a `bell::URLParser::urlDecode()`, y
`cspot_connect.cpp` borró su copia local y pasó a usar la de `bell`
directo (`#include "URLParser.h"` agregado, `<cctype>` ya no hace
falta en `cspot_connect.cpp` - la validación ahora vive del lado de
`bell`).

**Verificado**: `unit_tests` 27/27 (`URLParser.cpp` se recompiló con el
cambio, es host-testable). Chequeo standalone aparte (no parte de la
suite oficial) confirmando el comportamiento nuevo de
`urlDecode()` sobre casos malformados/truncados
(`"bad%zzend"`→sin tocar, `"trail%4"`→sin tocar, `"full%41B"`→`"fullAB"`)
- antes del fix, el primer caso habría inyectado un byte NUL en
silencio. `MDNSService.cpp` (ESP) y `cspot_connect.cpp` no son
host-testables (dependen de componentes ESP-IDF reales) - revisados a
mano, `blob->getDeviceName()` confirmado como el mismo string que
`deviceName` (pasa derecho por el constructor de `LoginBlob`), así que
usar `serviceName` dentro de `ensureResponderStarted()` es
equivalente al comportamiento viejo, no un cambio de valor.

## 53. `components/ui/cover_art.cpp` tenía su propio segundo stack HTTP+TLS (`esp_http_client`/`esp_crt_bundle`) - consolidado sobre `bell::HTTPClient`

Mismo pedido que §52, extendido a `components/ui/cover_art.cpp`: el
fetch de la carátula (`fetch_https()`) usaba `esp_http_client` +
`esp_crt_bundle_attach` directo - un segundo stack HTTP+TLS
completamente separado del `bell::HTTPClient`/`TLSSocket` que usa el
resto del proyecto (Login5, PUT de connect-state, CDN de audio,
context-resolve). Confirmado por grep que `esp_http_client` no se usa
en ningún otro lado del árbol - sólo acá.

**Por qué importaba, más que el caso de `cspot_connect.cpp`**: costo
real de flash (dos stacks HTTP+TLS independientes linkeados, con el
margen de flash ya documentado como ajustado - F12), y ninguno de los
fixes de esta sesión (TLS 1.3, keepalive de TCP, reintento sobre
conexión muerta, chunked encoding, bounds-check de headers) alcanzaba
este camino - corría sin auditar. Menos urgente de lo que suena, sí:
`fetch_https()` abre una conexión nueva por llamada (sin reuso), así
que no sufre la clase específica de bug de "conexión idle que se
degrada en silencio" que motivó gran parte de esta investigación.
`esp_crt_bundle_attach()` (lo que usaba) y `bell::X509Bundle` (lo que
usa `TLSSocket`) sacan del mismo bundle de certificados de ESP-IDF por
debajo - no se pierde ninguna capacidad de validación TLS al migrar.

**El truco real de esta migración no fue el código en sí, fue el
límite de componentes**: `HTTPClient.h` vive dentro de `bell`, que
`components/cspot/CMakeLists.txt` linkea deliberadamente como
`PRIVATE` (`target_link_libraries(${COMPONENT_LIB} PRIVATE cspot)`) -
`ui/cover_art.cpp` no puede simplemente incluir `HTTPClient.h`, ese
include path no se propaga a través del límite del componente ESP-IDF
`cspot`, a propósito (mantiene los internals de `bell`/`cspot`
encapsulados, sólo expone la API en C de `cspot_connect.h`). En vez de
forzar ese límite (agregar el include path de `bell` a mano en
`ui/CMakeLists.txt`, algo fràgil que alcanza directo a la estructura
interna de otro componente), se agregó una función nueva a la API
pública en C ya existente:

```c
// cspot_connect.h
int cspot_fetch_url(const char* url, uint8_t* buf, size_t buf_size);
```

Implementada en `cspot_connect.cpp` (que sí ve `bell::HTTPClient`,
está compilado dentro del componente `cspot` con el link privado)
usando `bell::HTTPClient::get(url)` + `.bytes()` (no
`.stream().read()` directo - así hereda transparentemente el soporte
de chunked encoding de §48 sin asumir cuál usa el CDN de imágenes de
Spotify). `cover_art.cpp::fetch_https()` quedó como un wrapper de una
línea sobre `cspot_fetch_url()`. Se le quitaron los includes de
`esp_http_client.h`/`esp_crt_bundle.h`, y `esp_http_client` se sacó de
`PRIV_REQUIRES` en `ui/CMakeLists.txt` (built-in de ESP-IDF, no
managed component - no hay entrada en ningún `idf_component.yml` que
tocar).

**Verificado con un build real de ESP32, no sólo host tests** (`source
$IDF_PATH/export.sh && idf.py build`, target real esp32s3 de este
proyecto) - limpio, sin errores, `cover_art.cpp`/`cspot_connect.cpp`/
`HTTPClient.cpp`/`URLParser.cpp`/todo el componente `ui` compilan y
linkean bien. 50% de flash libre tras el cambio - margen cómodo, sin
señal de haber empeorado el presupuesto de F12 (no se hizo un build
"antes" limpio para medir el delta exacto, pero el margen resultante
por sí solo descarta cualquier preocupación).

## 54. Auditoría de `idf_component_register()` en los 4 componentes reales del proyecto - 4 dependencias declaradas de más, retiradas

Pedido del usuario: revisar `REQUIRES`/`PRIV_REQUIRES` de cada
`idf_component_register()` real del proyecto (`main`, `components/
cspot`, `components/ui`, `components/wifi_manager` - no
`cspot/cspot/tests`, que es la suite de host, un proyecto CMake
aparte) buscando dependencias declaradas que ya no hagan falta.

**`main/CMakeLists.txt`** (`REQUIRES ui nvs_flash`) y
**`wifi_manager/CMakeLists.txt`** (`PRIV_REQUIRES esp_wifi esp_netif
nvs_flash`): todas genuinamente usadas (`nvs_flash_init()` en
`main.cpp`; `esp_wifi_init()`/`esp_netif_create_default_wifi_sta()`/
`nvs_open()` en `wifi_manager.c`) - sin cambios.

**`components/ui/CMakeLists.txt`**: `mbedtls` y `esp_timer` sin
ninguna referencia en todo el árbol de `components/ui/` (confirmado
por grep, incluido `fonts/`). `mbedtls` era un resabio de la vieja
`esp_crt_bundle_attach()` de `cover_art.cpp`, recién retirada en §53 -
`esp_crt_bundle` necesita `mbedtls` para el tipo de su firma
(`mbedtls_ssl_config*`), y sin ese código ya no hace falta.
`esp_timer` no tiene ninguna conexión visible con nada del
componente. Ambos retirados.

**`components/cspot/CMakeLists.txt`**: `mbedtls` y `pthread` en
`PRIV_REQUIRES` - a primera vista parecían necesarios de verdad
(`TLSSocket.cpp` incluye `mbedtls/ssl.h` directo, `PlayerEngine.cpp`/
`DealerClient.cpp`/etc. usan `std::mutex`/`std::condition_variable` sin
parar, que en ESP-IDF corren sobre el backend de threading en C++ del
componente `pthread`). Probado en serio en vez de asumido: se
retiraron ambos, y se forzó la recompilación completa de
`TLSSocket.cpp` y `MercurySession.cpp` (`touch` + rebuild) - las dos
compilaron y linkearon limpio sin ellos, confirmando que ya están
visibles de forma transitiva (lo más probable, vía las cadenas de
dependencia propias de `esp_ringbuf`/`esp_driver_i2s`/
`esp_http_server`, o la visibilidad global de componentes de ESP-IDF
para este grafo de build en particular). Retirados también.

**Verificado con un build real de ESP32, limpio de punta a punta**
(`idf.py fullclean && idf.py build`, ~2000 pasos desde cero, no
incremental - la verificación más fuerte posible acá, ya que un build
incremental anterior no forzó la recompilación de `bell` y hubiera
dado un falso positivo). Compiló y linkeó sin errores. Tamaño del
binario idéntico al de antes de esta sección (`0x1fce40` bytes, 50%
libre) - confirma que estas cuatro dependencias nunca aportaron código
real enlazado, sólo visibilidad de headers/símbolos que ya estaba
disponible por otro lado.

## 55. Cambios de volumen remoto nunca se le avisaban al cliente - causa raíz real del "siempre muestra 50%"

**Síntoma reportado por el usuario**: el volumen SÍ funciona (el audio
en sí cambia de nivel), pero la app de Spotify siempre muestra 50% sin
importar cuántas veces se cambie remotamente.

**Causa encontrada, comparando contra `go-librespot`**:
`PlayerEngine::handleSetVolume()` (llamada al recibir
`hm://connect-state/v1/connect/volume` del Dealer) actualizaba
`ctx->config.volume` y disparaba el evento local
(`sendEngineEvent(EventType::VOLUME, volume)`, que sólo ajusta el
audio sink) - pero **nunca disparaba un PUT**. `buildDeviceInfo()`
(usada por cualquier PUT) sí lee `ctx->config.volume` fresco cada vez,
así que el valor nuevo únicamente le llegaba a la app si, por
casualidad, algún PUT **no relacionado** (ej. un cambio de track)
pasaba a ocurrir después - si no, la app se quedaba mostrando lo
último que sí se llegó a mandar (típicamente `initial_volume`, de ahí
el "siempre 50%") para siempre, sin importar cuántos cambios de
volumen remotos pasaran después.

`go-librespot` (`daemon/controls.go`, `updateVolume()`/
`volumeUpdated()`) sí manda un PUT con `PutStateReason_VOLUME_CHANGED`
cada vez que el volumen cambia - de cualquier origen (Spotify Connect,
su propia API REST, o un mixer local), sin excepción.

**Detalle importante encontrado al portear esto, no obvio a primera
vista**: `putConnectState()` de `go-librespot` manda
`IsActive: p.state.active` (su propio estado cacheado, real) en TODOS
los PUT, incluido el de volumen. El mecanismo equivalente de `cspot`
para reenviar el estado cacheado
(`PlayerEngine::updatePlayerState()`, ya usado por el caso
`update_context`) en cambio **fuerza `is_active=true` sin condición**
- deliberado, documentado en el comentario de `runTask()`, porque ese
método sólo se pensó para llamarse desde un evento de reproducción
real. Reusarlo tal cual para el PUT de volumen hubiera anunciado el
dispositivo como activo sólo por tocar el volumen, incluso sin nada
sonando todavía - una regresión nueva, distinta a la que se estaba
arreglando. Corregido con un guard: el PUT de volumen sólo se manda si
`isActiveDevice` ya es `true` (mismo gate que el `p.state.active` real
de `go-librespot`) - si el dispositivo todavía no está activo, no hay
nada que anunciar todavía, el volumen le llega a la app gratis en el
PUT que sí lo active más adelante.

**Corregido**:
- `PlayerEngine::updatePlayerState()` gana un quinto parámetro
  opcional, `reason` (default `PLAYER_STATE_CHANGED`, sin tocar a
  ningún llamador existente) - se propaga a través del mecanismo de
  coalescing/rate-limit existente (`pendingReason`,
  `PlayerEngine.h`) hasta `runTask()`, que ahora usa el `reason`
  recibido en vez de tener `PLAYER_STATE_CHANGED` fijo a mano.
- `handleSetVolume()` ahora, tras actualizar `ctx->config.volume` y
  antes de retornar, si `isActiveDevice` es cierto: relee el estado
  cacheado (`lastKnownTrackUri`/`lastKnownDurationMs`/`isPlayingState`
  bajo `engineMutex`, `getPositionMs()` aparte - mismo patrón exacto
  que ya usa el caso `update_context`, sin tomar el lock dos veces) y
  llama `updatePlayerState(..., connectstate_PutStateReason_VOLUME_CHANGED)`.

**Verificado con un build real de ESP32** (`idf.py build`,
incremental sobre el build limpio de §54) - compiló y linkeó sin
errores, sólo ~192 bytes más de binario (0x1fce40 → 0x1fcf00), 50%
libre. `PlayerEngine.cpp` no está en el set de host tests
(mismo límite de siempre) - revisado a mano. Pendiente hardware:
cambiar el volumen desde el cliente remoto y confirmar que la app
refleja el nuevo valor de inmediato, sin necesitar ningún otro evento
en el medio.

## 56. Podcast que no cargaba (CDN 404) - investigado un ángulo distinto: episodios alojados externamente, agregada sólo visibilidad (no un fix a ciegas)

**Contexto**: un episodio de podcast (`file` con formatos
`MP4_128_DUAL, MP4_128, OGG_VORBIS_96, AAC_160, AAC_160, MP4_128,
OGG_VORBIS_96` - sin MP3) fallaba con `CDN header request got HTTP 404
instead of 206` tras conseguir audio key y URL de CDN normalmente.
Primer análisis: la cadena de fallback de formato de `TrackQueue.cpp`
sólo tiene dos escalones (match exacto de `ctx->config.audioFormat`,
que siempre es un `OGG_VORBIS_*` → fallback a `OGG_VORBIS_96` → si
tampoco hay Vorbis, fallback a MP3) - sin ningún escalón para AAC/MP4,
así que terminó eligiendo el único formato "conocido" (`OGG_VORBIS_96`)
aunque `AAC_160`/`MP4_128*` estuvieran disponibles en la misma lista.
Además, aunque los hubiera elegido, `cspot` no puede decodificarlos de
todas formas: `BELL_CODEC_AAC OFF` (`components/cspot/CMakeLists.txt:45`)
y, más contundente, `EncodedAudioStream.cpp::decodeFrameAAC()` tiene el
cuerpo real **comentado** (detecta AAC pero nunca lo decodifica) - sin
soporte de contenedor MP4 en absoluto tampoco.

**El usuario trajo un ángulo distinto**: [issue #135 de
`librespot-python`](https://github.com/kokarare1212/librespot-python/issues/135)
([comentario relevante](https://github.com/kokarare1212/librespot-python/issues/135#issuecomment-1168629286))
- algunos episodios de podcast (típicamente licenciados/exclusivos) no
están alojados en el CDN de Spotify en absoluto, sino en un host de
terceros (ej. megaphone.fm), accesibles vía un campo
`external_playback_url`/similar en la metadata del episodio, **en vez
de** (no además de) el mecanismo normal de `file`/audio-key/CDN.

**Verificado en `go-librespot`**: el campo real (en su schema completo,
no el recortado de `cspot`) es `Episode.external_url` (campo 83,
`optional string`) - existe en el protobuf generado
(`Episode.GetExternalUrl()`), **pero no se lee en ningún lado del
código Go escrito a mano** (`daemon/*.go`, `player/*.go`) - sólo existe
porque viene del schema oficial completo. Es decir: `go-librespot`
**tampoco** resuelve este caso - no hay una implementación de
referencia para portar acá. El nombre `is_externally_hosted` del issue
no aparece como campo separado en este schema (probablemente es de la
Web API pública de Spotify, una superficie distinta a la metadata de
Mercury que usan `cspot`/`go-librespot`).

**Decisión, siguiendo el mismo criterio que la decisión de gzip
(§6.3 del histórico original)**: no implementar un fetch a ciegas
contra `external_url` sin haber confirmado primero que Spotify
realmente lo manda para episodios como el que falló - no hay ninguna
referencia que confirme el formato de esa respuesta (¿redirect?
¿stream crudo?), y el propio `file` de este episodio SÍ tenía entradas
que resolvieron audio key + URL de CDN con normalidad (sólo la
descarga final del formato elegido dio 404) - ambigüedad genuina entre
"este episodio es externo de verdad" y "sólo el formato Vorbis
elegido, específicamente, no tiene objeto real en el CDN". Agregado en
cambio, sólo para tener visibilidad real la próxima vez que esto
pase:

- `metadata.proto`: `Episode.external_url = 83` (mismo número de campo
  que el schema real, para no romper compatibilidad de wire si algún
  día se usa).
- `metadata.options`: `Episode.external_url type: FT_POINTER` (mismo
  patrón que `Episode.name`).
- `TrackQueue.cpp`: un log (`"Episode has external_url: %s"`) si el
  campo viene poblado - nada más, no se cambia el flujo de reproducción
  todavía.

**Verificado con un build real de ESP32** (`idf.py build`) - el
regenerador de nanopb corre automáticamente como parte del build
normal (`nanopb_generate_cpp()`, `components/cspot/cspot/CMakeLists.txt:30`
- no hizo falta ningún venv manual esta vez, a diferencia de §36).
Confirmado el campo nuevo en el `.pb.h` generado
(`Episode_external_url_tag`, `char *external_url`). Compiló y linkeó
limpio, 50% de flash libre.

**Pendiente**: la próxima vez que un podcast falle así en hardware,
revisar el log por la nueva línea - si aparece, confirma la teoría del
issue y recién ahí vale la pena diseñar el fetch directo (probablemente
un GET HTTPS plano, sin audio-key, posiblemente contra un host
completamente distinto a `audio-ak.spotifycdn.com`); si nunca aparece
para episodios que sí fallan, el problema es puramente el hueco de
fallback de formato/codec ya documentado arriba, y la solución pasa
por ahí en cambio (agregar un decoder AAC real + demux MP4 - trabajo
mayor, evaluar sólo si de verdad hace falta).

**Actualización - probado con el episodio real, `external_url` nunca
apareció**: el mismo episodio ("Episodio 2: Peinado a la gomina",
mismo `file_id` `6b9dd9949...`) se reprodujo/falló de nuevo en
hardware con el log de diagnóstico ya activo - **ningún** `"Episode
has external_url: ..."` en el log. Descarta la teoría de alojamiento
externo para este episodio puntual - el campo está genuinamente vacío,
no es que el log lo esté pasando por alto.

**Encontrado revisando el propio `go-librespot` más a fondo, a pedido
del usuario**: `Capabilities.supports_external_episodes` (campo 17)
- `daemon/player_state.go:143`: `SupportsExternalEpisodes: false, //
TODO: support external episodes`. El campo **ya existía** en el
`connectstate.proto` recortado de `cspot`
(`Capabilities.supports_external_episodes = 17`, mismo número que en
el schema real) pero nunca se tocaba en `buildDeviceInfo()` - quedaba
en `false` por el zero-init de nanopb, funcionalmente idéntico al
`false` explícito de `go-librespot`. Ningún cambio de comportamiento
real ahí - ambos clientes ya le dicen a Spotify "no soporto episodios
externos", de la misma forma, sólo que uno lo hace explícito y el otro
implícito.

**Experimento pedido por el usuario**: seteado `caps.
supports_external_episodes = true;` a mano en `buildDeviceInfo()`
(`PlayerEngine.cpp`), sólo para observar si Spotify empieza a
mandar `Episode.external_url` para episodios como este una vez que el
dispositivo dice que SÍ puede manejarlos - `cspot` todavía no tiene
ningún camino para *actuar* sobre ese campo aunque llegue (el log de
§56 es sólo diagnóstico), así que esto es pura observación, no se
espera que arregle la reproducción por sí solo todavía. Revertir a
`false` si no cambia nada, o si causa cualquier otra regresión (el
servidor podría empezar a asumir capacidades que el dispositivo
todavía no puede cumplir).

**Verificado con build real de ESP32** - compiló y linkeó limpio, 50%
de flash libre, sin cambio de tamaño de binario respecto al build
anterior (el campo ya existía en el proto, esto sólo cambia un valor
en tiempo de ejecución). Pendiente hardware: repetir la prueba con
este flag en `true` y revisar si el log de `external_url` (§56)
finalmente aparece para este mismo episodio.

**Descartado este camino - confirmado por
[`librespot-org/librespot#818`](https://github.com/librespot-org/librespot/issues/818)**:
el issue de la propia implementación de referencia en Rust reporta el
mismo síntoma general (carga rota de podcasts) sin ninguna mención de
`external_url`/alojamiento externo como causa ni como solución - la
conclusión ahí es que Spotify rompió esto del lado servidor
("Spotify have once again broken podcasts with librespot"),
independiente de lo que el cliente declare soportar. Esto confirma que
no es algo controlable por un capability flag - revertido
`caps.supports_external_episodes` a su default `false` (mismo estado
que antes del experimento, sin cambio de comportamiento). El log de
diagnóstico agregado en §56 (`"Episode has external_url: ..."`) queda
igual - es barato y sigue siendo útil por si aparece para OTRO
episodio en el futuro, pero ya no se persigue activamente esta teoría
para el caso de "Episodio 2: Peinado a la gomina" en particular. La
explicación que se mantiene en pie para ese episodio puntual sigue
siendo el hueco de fallback de formato/codec (sin AAC real, sin demux
MP4) documentado más arriba en esta misma sección.

**Verificado con build real de ESP32** - revert compiló y linkeó
limpio, mismo binario/margen de flash que antes del experimento.
