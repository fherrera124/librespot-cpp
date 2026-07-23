# Dealer WebSocket + Connect-State

Referencia del estado actual de la migración de SPIRC/Mercury a Dealer
WebSocket + connect-state HTTP. La migración está **completa**: SPIRC
fue retirado, `PlayerEngine` es el único motor de reproducción.

Este documento está organizado por tema, no cronológicamente. El
detalle histórico completo (línea de tiempo día a día, hipótesis
descartadas, la investigación completa del cierre del Dealer WS) está
en `dealer_websocket_migration_historial.md` — los comentarios en el
código que dicen `// ver docs/dealer_websocket_migration.md §N`
apuntan a la numeración de **ese** archivo histórico, no a las
secciones de este.

---

## 1. Arquitectura

SPIRC (frames protobuf sobre Mercury pub/sub, `hm://remote/user/.../v23`)
está completamente eliminado — no coexiste con nada, fue reemplazado
por un diseño de dos mitades, confirmado independientemente en
librespot (Rust) y `go-librespot` como el diseño real que usa Spotify
hoy, no una idiosincrasia de un solo cliente:

| Antes (SPIRC/Mercury) | Ahora |
|---|---|
| Recibir comandos remotos | **Dealer WebSocket** (push JSON, entrante) |
| Publicar el propio estado | **PUT HTTP** a `spclient` (saliente, protobuf, nunca por el WS) |
| Pub/sub en Mercury | Eliminado |

**Clases y responsabilidades**:

- **`DealerSession`** (`bell::Task` propia, 32KB/PSRAM): dueña del ciclo
  de vida de la conexión WS. Resuelve el host (`ApResolve`), pide token
  (`Login5Client`), conecta vía `WebSocketTransport`, parsea el
  envelope JSON, rutea por URI/`message_ident`, manda/verifica ping-
  pong, reconecta con backoff y token fresco. Dueña de
  `PlayerEngine`.
- **`PlayerEngine`** (su propia `bell::Task`, 32KB/PSRAM):
  reemplaza a `SpircHandler`. Dueña directa de `TrackQueue`/
  `TrackPlayer` (el motor de reproducción en sí, no hay dos motores
  separados). Arma y manda `PutStateRequest`, maneja `ClusterUpdate`,
  comandos de `player/command`, volumen, transfer/play, rotación de
  `session_id`/`playback_id`, PUTs con rate-limit/coalescing.
- **`bell::WebSocketTransport`** (interfaz) con una única implementación
  (`bell/main/io/WebSocketTransport.cpp` - movida ahí desde `src/`
  el 2026-07-18, ya que no tenía ninguna dependencia real de Dealer ni
  de ESP32: el tuning específico del Dealer (`PING_INTERVAL_MS`/
  `PING_TIMEOUT_MS`/`MAX_MESSAGE_SIZE`) ahora son parámetros de
  `create()`, con los valores calibrados viviendo en `DealerSession.cpp`
  en vez de hardcodeados en el transporte): RFC 6455 propio sobre
  `bell::TLSSocket`, un solo hilo, sin mutex, **no thread-safe** (ver
  comentario de cabecera del archivo). Rama `wss://` idéntica en firmware y host;
  una rama host-only adicional (`ws://` plano, sin TLS, vía
  `WsSocket`/`PlainSocket`) existe solo para que
  `ws_transport_echo_test` pueda validar el framing contra un servidor
  de eco en texto plano, sin montar TLS para una prueba local. El
  componente oficial `esp_websocket_client` de ESP-IDF se probó, tuvo
  bugs reales contra el Dealer real, y fue retirado por completo
  (el archivo que lo probaba en A/B, deshabilitado, fue borrado
  después — ya no aporta nada, ver §28/§29 del historial).
- **`Login5Client`**: OAuth (clienttoken → login5 → token de usuario).
- **`ContextResolver`**: cliente HTTP de `context-resolve/v1/{uri}`.

**Eliminado**: `SpircHandler`, `PlaybackState`, `spirc.proto`/
`.options`, toda la maquinaria de suscripción Mercury
(`executeSubscription`, mapa de `subscriptions`, SUB/UNSUB/SUBRES).
`spircMutex` también desapareció — `TrackQueue`/`TrackPlayer` protegen
su propio estado internamente ahora.

**Mercury sigue vivo** para: autenticación de sesión AP
(`authenticate()`), `requestAudioKey()` (no tiene equivalente HTTPS),
country code, sync de tiempo, y el único GET de metadata
(track/album/artist) que sigue sin necesidad de migrar a HTTPS.

---

## 2. Protocolo del Dealer — lo que hay que saber para no reintroducir un bug

- **Envelope**: frames de texto WS, JSON, discriminado por `"type"`:
  `"message"` (push, `{headers, payloads:[...], uri}`), `"request"`
  (RPC, `{headers, message_ident, key, payload}`), `"ping"`/`"pong"`.
  Respuesta a un `"request"`: `{"type":"reply","key":...,
  "payload":{"success":bool}}` — **toda** request necesita respuesta o
  el cliente real queda esperando un timeout.
- **El payload de un `"request"` sin comprimir NO viene envuelto en
  `{"compressed":"<base64>"}`** — viene directo como el objeto comando
  ya decodificado. Ese wrapper sólo aplica al caso gzip (no soportado,
  ver §5).
- **Ruteo**: `"message"` por prefijo de URI, `"request"` por
  `message_ident` exacto. URIs clave: `hm://pusher/v1/connections/`
  (entrega `Spotify-Connection-Id`), `hm://connect-state/v1/cluster`
  (`ClusterUpdate` protobuf), `hm://connect-state/v1/connect/volume`,
  `hm://connect-state/v1/player/command` (el único `"request"`,
  comandos reales).
- **`x-spotify-connection-id`**: se obtiene del primer push, se manda
  como header en cada PUT posterior — es el mecanismo que correlaciona
  la sesión WS con el HTTP PUT.
- **El ping JSON de aplicación (`{"type":"ping"}` cada 30s) es
  obligatorio — los frames de control WS solos NO alcanzan** para
  mantener la conexión no-idle a los ojos del backend. `DealerSession`
  además vigila que los pongs realmente lleguen (`jsonPongDeadline`,
  timeout ~40s) y fuerza una reconexión si dejan de llegar — mismo
  mecanismo que `go-librespot` (`pingTicker()`/`timeSinceLastPong()`).
  Es el único mecanismo de reconexión proactiva que se mantiene hoy
  (ver §4).
- **`session_id` rota por sesión de reproducción** (por contexto
  cargado), no por vida del dispositivo — regenerar en `play`/contexto
  nuevo, adoptar `original_session_id` si `transfer` lo trae.
- **`is_playing` e `is_paused` NO son mutuamente excluyentes**:
  `is_playing` significa "hay una sesión cargada" (sigue en `true`
  durante una pausa); `is_paused` + `playback_speed=0` es lo que
  indica que el audio no fluye. Poner `is_playing = !is_paused` apaga
  el botón de resume en el cliente real.
- **Cada PUT debe repetir `last_command_message_id`/
  `last_command_sent_by_device_id`** del último comando manejado — es
  cómo el cliente que mandó el comando correlaciona el cluster con su
  propio comando. El `{"success":true}` de la respuesta WS sola no
  alcanza.
- **`playback_id` es hex, no base64** (a diferencia de `session_id`,
  que sí es base64 en ambas referencias).
- **`update_context` nunca debe fallar** — si la URI no coincide,
  ignorar en silencio; si coincide, aplicar metadata/restricciones y
  mandar un PUT propio (no alcanza con responder `success`).
- **`transfer` puede venir genuinamente vacío** (dispositivo
  seleccionado sin nada sonando en ningún lado) — tratar como éxito,
  no error, y de todos modos marcar `is_active=true`. Chequear
  `raw.empty()` tras decodificar, no la presencia del campo JSON
  (`pb_decode()` sobre 0 bytes "tiene éxito" trivialmente). También
  puede venir sin contexto/cola pero con un `current_track` real —
  cargar una cola de un solo track en ese caso.
- **`play` manda el contexto como JSON plano** (`command.context.uri`),
  no protobuf-base64 como `transfer`.
- **PUTs con rate-limit de 200ms** (`PUT_MIN_INTERVAL_MS`, mismo valor
  que `go-librespot`), coalescing "el último gana". El PUT de registro
  (`NEW_DEVICE`) queda afuera de este límite.
- **`pbPutString()` no tiene bounds-check propio** — cualquier string
  que venga de JSON del servidor (no generado internamente) debe
  truncarse antes (`truncateForPb()`) o corrompe campos adyacentes del
  struct, produciendo protobuf inválido que rompe **todos** los PUTs
  siguientes hasta reiniciar el estado.
- **`connectstate.proto` está deliberadamente recortado** frente al
  schema real de Spotify (usa proto3, sólo los campos que
  `PlayerEngine` realmente lee/escribe) — los números de campo
  coinciden con el schema real para que lo omitido se saltee limpio en
  el decode, no rompe compatibilidad de wire. Ver §5 para el detalle de
  qué falta.
- **gzip no está soportado** — `Capabilities.supports_gzip_pushes =
  false` se manda a propósito para que el servidor jamás comprima los
  pushes. Si algún día hace falta, hay que revisar esa flag primero.
- **Hay un delay de servidor de ~17s entre que el usuario toca play y
  llega el comando real** — confirmado que no es un bug de `cspot`
  (nada bloquea del lado cliente en el medio), es latencia del backend
  de Spotify, no arreglable desde acá.
- **`LWIP_MAX_SOCKETS=16`**: la cantidad de conexiones persistentes
  concurrentes (AP/Mercury, Dealer WS, PUT, ContextResolver, CDN) más
  el httpd de zeroconf deja poco margen con el default de 10 — ya causó
  un `esp-tls: Failed to create socket` real.
- **`TCP_NODELAY` en `TLSSocket`**: sin esto, Nagle agregaba hasta
  ~200ms por cada request/response HTTPS corto (Login5, PUTs,
  context-resolve).

---

## 3. La investigación del cierre del Dealer WS — resuelta

**Síntoma original**: el Dealer WS se cerraba (`peer sent WebSocket
close`) después de un rato de idle seguido de un comando real, y la
app de Spotify mostraba el dispositivo como desconectado aunque
siguiera reproduciendo audio localmente.

**Causa raíz real, confirmada en hardware**: `cspot` nunca activaba
**TCP keepalive** en ningún socket (`TLSSocket`, `PlainConnection`) -
ni una sola vez, en ningún lado del proyecto - pese a que el lwIP de
ESP-IDF lo soporta completo (`LWIP_TCP_KEEPALIVE=1`). `go-librespot`
tiene esto gratis desde siempre, sin que nadie lo haya pedido a
propósito, porque el `http.DefaultTransport` de Go trae
`KeepAlive: 30s` en su dialer por default. Sin keepalive, un camino de
red silenciosamente semi-muerto durante el idle (lo más probable: un
NAT/tracker de conexión de operador que expira su entrada por falta de
tráfico TCP real) queda sin detectar hasta que el servidor reacciona al
próximo tráfico real - lo que parecía "Spotify reciclando la conexión"
en realidad era Spotify reaccionando a un camino que ya estaba muerto.

**Fix**: `TLSSocket::open()` y `PlainConnection.cpp` activan
`SO_KEEPALIVE=1` + `TCP_KEEPIDLE=30` + `TCP_KEEPINTVL=10` +
`TCP_KEEPCNT=3` (peor caso ~60s para detectar un peer muerto).
**Confirmado en hardware: el cierre ya no ocurre.**

Antes de encontrar esto, se identificaron y corrigieron tres bugs
reales que agravaban el síntoma (compuestos, no la causa raíz del
cierre en sí):

1. **Bootstrap de TLS 1.3 fallaba**: mbedTLS 4.0 devuelve
   `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET` cuando el servidor
   manda un `NewSessionTicket` post-handshake (TLS 1.3 puede mandarlo
   en cualquier momento tras el handshake) - `TLSSocket::read()` no
   conocía ese código y lo trataba como error fatal. Corregido
   reintentando mientras `ret == RECEIVED_NEW_SESSION_TICKET` **o**
   `ssl.state == MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET` (las dos
   condiciones hacen falta - mbedTLS puede devolver un `0` llano,
   indistinguible de EOF, mientras sigue atascado en ese estado).
2. **Ese mismo loop podía quedar atascado leyendo para siempre**: si un
   ticket empezaba a llegar pero la conexión moría a mitad de camino,
   `ssl.state` quedaba parado ahí para siempre y el loop reintentaba
   sin parar, ignorando cualquier timeout real. Corregido: un
   `MBEDTLS_ERR_SSL_TIMEOUT` genuino corta el loop siempre, sin
   importar `ssl.state`.
3. **`bell::SocketBuffer::sync()`/`::xsputn()` colgaban para siempre en
   una escritura fallida** (el bug más importante de los tres): el
   código comparaba `bw < 0`, pero `TLSSocket::write()` siempre
   recorta los errores a `0` (nunca negativo) - una escritura
   genuinamente fallida se veía igual que "escribí 0 bytes, reintentar"
   y el loop nunca terminaba. Esto colgaba para siempre el hilo entero
   de `PlayerEngine` en una conexión PUT reusada y muerta -
   explicando por qué, tras una reconexión del Dealer, la app nunca se
   enteraba de que el dispositivo seguía vivo (ningún PUT volvía a
   mandarse). Corregido: `bw < 0` → `bw <= 0`.

**Mecanismos de reconexión proactiva agregados mientras se perseguía
esto, luego retirados** una vez confirmado el keepalive como la causa
real - `go-librespot` no tiene equivalente para ninguno de los dos:

- `PlayerEngine`: reciclado proactivo de la conexión PUT si
  llevaba >45s idle - retirado, vuelve al patrón reactivo simple
  (reusar, y si tira excepción, reconectar en el próximo intento).
- `CDNAudioFile`: mismo patrón para la conexión CDN (>20s idle) -
  retirado, mismo motivo.
- **Se mantiene**: el watchdog de pongs perdidos de `DealerSession` (ver
  §2) - es el único de los tres con equivalente real confirmado en
  `go-librespot`.

---

## 4. Experimentos revertidos — no reintentar sin releer el historial

- **`SO_LINGER` (cierre abortivo por RST)**: causó un loop de
  reconexión nuevo (cada 10-20s desde el arranque). Revertido.
- **Ping JSON cada 5s** (en vez de 30s): descartado antes de probar en
  hardware al confirmar que librespot-rust usa 30s/3s y no sufre el
  bug - un cliente que pinguea más lento no debería andar peor si el
  ping fuera la causa.
- **`esp_websocket_client` (oficial de ESP-IDF) como A/B**: mismo
  cierre, más un bug propio (`unexpected data readable on socket`) -
  confirma que el cliente RFC 6455 propio fue la decisión correcta.
- **Diseño de dos hilos para el ping** (`DealerPingTask` separada +
  mutex en `WebSocketTransport.cpp`, replicando el goroutine de ping
  independiente de `go-librespot`): el cierre persistió igual (un pong
  llegó 14ms antes de un cierre, refutando la teoría de "el cliente
  queda en silencio mientras procesa"), y además gatilló un crash real
  de `bell::Task` (el destructor libera el stack de FreeRTOS mientras
  la tarea todavía puede estar terminando su propia limpieza - ver §6).
  Revertido por completo vía `git checkout`.
- **TLS 1.3 en sí**: no es la causa del cierre (confirmado corriendo
  con TLS 1.3 ya funcionando correctamente, el cierre seguía igual) -
  se mantiene activado porque es correcto, no por esto.

---

## 5. Limitaciones conocidas / alcance diferido

- **gzip**: no soportado, ver §2.
- **`connectstate.proto` recortado**: falta el mapa completo de
  dispositivos en `Cluster`, `Cluster.player_state`, la mayoría de las
  razones de `Restrictions` (sólo 3 de ~25), varios flags de
  `Capabilities`. `prev_tracks`/`next_tracks` tapados en 3 (contra 32
  de `go-librespot`) por presupuesto de stack, y sólo llevan `uri`
  (`TrackReference` no tiene más campos).
- **Shuffle**: no implementado, nunca lo estuvo - no es una regresión.
- **`HTTPClient` no soportaba `Transfer-Encoding: chunked`** hasta la
  auditoría de `bell` (§7) - no hay evidencia de que Spotify lo use,
  pero era un bug latente real, ya corregido.
- **Latencia del primer play (~6-9s)**: cadena secuencial
  (context-resolve → metadata Mercury → audio key → CDN+TLS) -
  optimizada parcialmente (`TCP_NODELAY`, cacheo de host), no más allá
  de eso a propósito - es cosmético, no funcional.

---

## 6. Auditoría de las clases de conexión de `bell`

Revisión completa de `TCPSocket`, `SocketStream`, `HTTPClient`,
`URLParser` buscando la misma clase de inconsistencias que ya había
causado bugs reales (asimetría entre las dos implementaciones de
`bell::Socket`, manejo de errores inconsistente). Ninguno de estos se
dispara hoy en producción (todo `cspot` usa `https://`), pero son
bugs reales, ya corregidos:

1. `TCPSocket::read()`/`write()` no recortaban errores negativos a 0
   (a diferencia de `TLSSocket`) - dependía de que el patrón de bits
   sobreviviera un viaje `size_t`→`ssize_t` no garantizado.
2. `TCPSocket::open()` perdía el `addrinfo*` si `connect()` fallaba.
3. `TCPSocket` no tenía ningún timeout de lectura/escritura - agregado
   `SO_RCVTIMEO`/`SO_SNDTIMEO` de 15s (con rama separada para Windows,
   que usa `DWORD` en vez de `timeval`).
4. `TCPSocket::sockFd` sin inicializar - `getFd()` antes de `open()`
   devolvía basura en vez de `-1`.
5. El constructor de 3 argumentos de `SocketBuffer` ignoraba `isSSL`
   por completo (siempre abría TCP plano).
6. `HTTPClient::rawRequest()`: el header declaraba
   `(method, url, ...)`, la implementación real era `(url, method,
   ...)` - compilaba igual porque ambos son `std::string`.
7. Sin soporte de `Transfer-Encoding: chunked` - implementado completo
   (RFC 7230 §4.1), verificado byte a byte contra un servidor HTTP
   chunked real.
8. `std::stoi()` sin capturar sobre `Content-Length`/`Content-Range` -
   un valor malformado del peer se saltaba la lógica de reintento de
   `rawRequest()`.

Dos hallazgos más, de revisar los commits del PR upstream
[feelfreelinux/bell#48](https://github.com/feelfreelinux/bell/pull/48)
(mismo PR de donde salió el intento de `SO_LINGER`, §4) - el propio
proyecto upstream había encontrado la misma familia de problemas
independientemente:

9. `SocketStream.cpp`: `sync()`/`underflow()`/`xsgetn()` no chequeaban
   `internalSocket == nullptr` antes de usarlo - un stream nunca
   abierto o ya cerrado producía un null-pointer-dereference (crash
   duro) en vez de una excepción atrapable. Agregado el chequeo en las
   tres.
10. `HTTPClient.cpp`: los punteros `name`/`value` que devuelve
    `phr_parse_response()` (apuntan dentro de `httpBuffer`) se usaban
    sin validar que cayeran dentro del buffer real. Agregado el
    chequeo de límites.

---

## 7. Otros hallazgos reales, no relacionados al cierre del WS

- **`PlayerEngine`/`DealerSession` deben correr en su propia
  `bell::Task` (32KB, PSRAM)** - nunca HTTPS/TLS síncrono desde la
  tarea de quien llama. Dos crashes reales por violar esto: una tarea
  de 8KB insuficiente para un handshake TLS, y un PUT síncrono
  disparado desde la tarea de 4KB de los botones físicos.
- **`bell::Task`**: el destructor libera el stack de FreeRTOS
  inmediatamente al destruirse el objeto C++, pero la tarea de
  FreeRTOS todavía puede seguir corriendo brevemente sobre ese stack
  después (timer de limpieza, `vTaskDelete`) - crear/destruir
  `bell::Task` seguido (ej. una por reconexión) puede crashear
  (`Guru Meditation Error`, patrón `0xfefefefe`). No arreglado en la
  clase en sí - evitar tareas de vida corta.
- **Una conexión HTTP keep-alive reusada necesita el body drenado
  siempre**, éxito o error, antes de la siguiente request - si no, la
  siguiente lectura arranca en medio del body anterior
  (`Cannot parse http response`).
- **`HTTPClient::rawRequest()` manda `Accept: */*` fijo** - si el
  llamador manda su propio `Accept`, quedan dos headers duplicados y
  el edge de Spotify responde `200` con body vacío en silencio.
  Corregido: sólo manda el default si el llamador no puso uno propio.
- **El buffer de reensamblado del Dealer WS debe respetar el flag
  `FIN`** de cada frame, no sólo el largo acumulado - y el chequeo de
  tamaño máximo debe ser acumulativo entre fragmentos, no sólo del
  primer frame (subido de 32KB a 256KB tras perder en silencio un
  cluster update real de 54KB).
- **`sendPutStateRequest()` se llama desde dos tareas distintas**
  (registro desde `DealerSession`, estado normal desde la propia tarea
  de `PlayerEngine`) - necesita `putMutex` serializando toda la
  función, no sólo cachear campos.
- **`DealerSession::runTask()` no debe ejecutar `handlePlayerCommand()`
  inline** - puede bloquear en HTTP (context-resolve) y detiene la
  lectura/pong del WS mientras tanto. Separado en una `CommandWorker`
  con tarea propia; comunicación por dos colas
  (`pendingCommands`/`pendingReplies`), nunca directo, porque
  `WebSocketTransport` no es thread-safe (mbedTLS) y sólo el hilo de
  `runTask()` puede tocar `transport`. Ver docs/aprendizaje.md
  2026-07-18.
