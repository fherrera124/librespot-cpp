# Arquitectura de despacho de connect-state PUT: análisis y plan de mejora

Notas de una revisión arquitectónica de `ConnectStateHandler` (rama `dev`).
Cubre: qué hace cada clase relevante, cómo despacha el PUT de
connect-state, y un plan de mejora concreto. Contexto: surge después del
rediseño de esta sesión que movió todo el envío de PUT a un único emisor
(ver comentario de cabecera de `ConnectStateHandler.h` y de
`putStateLocked()`/`runTask()` en el `.cpp`).

## 1. Resumen de la arquitectura

Tres capas de hilos alrededor del connect-state:

1. **`DealerClient`** — WebSocket, hilo propio. Parsea el JSON del mensaje y
   lo postea a `EventLoop` (`DEALER_MESSAGE`/`DEALER_REQUEST`). No procesa
   nada él mismo.
2. **`EventLoop`** (`bell::Task` propio) — bus de pub/sub genérico, un solo
   hilo. `post()` encola desde cualquier hilo; `taskLoop()`
   (`EventLoop.cpp`) desencola y llama al handler registrado
   *sincrónicamente*, uno por vez. Todos los comandos de player
   (`handlePlayerCommand` y sus derivados: transfer/play/pause/skip/
   update_context) corren acá, incluyendo el fetch de red de
   `trackQueueHandler->loadContext()`.
3. **`ConnectStateHandler`** (`bell::Task` propio) — dueño exclusivo del
   envío de PUT. **Ningún** handler manda el PUT inline: todos solo mutan
   `putStateRequestProto` bajo `putStateMutex` y marcan `putStatePending`.
   `runTask()` (sobreescrito - reemplazó el `taskLoop()` con poll fijo de
   50ms original, ver commit "Replace the 50ms poll with condition_variable
   in ConnectStateHandler") es el único punto que codifica y llama a
   `spClient->putConnectStateRaw()`, fuera del lock.

## 2. Catálogo de clases

| Clase | Responsabilidad | Hilo propio |
|---|---|---|
| `Session` | Composition root: crea y conecta todas las piezas de una sesión (auth, AP, dealer, connect-state, streaming). | No (orquesta desde `runPoller()`) |
| `EventLoop` | Bus de pub/sub genérico (`post`/`registerHandler`), único punto de serialización para comandos de player, eventos de cola, claves de audio, etc. | Sí |
| `ConnectStateHandler` | Interpreta comandos de player, es dueño del proto `PutStateRequest` completo, decide cuándo mandar un PUT (scheduling + rate-limit) y lo codifica/envía. Envuelve además una `TrackQueueHandler` propia. | Sí (`runTask()`, sobreescrito - solo para el envío) |
| `TrackQueueHandler` (interfaz) / `DefaultTrackQueueHandler` | Contexto/cola/índice/ventanas next-prev de tracks. Interfaz abstracta con una única implementación real — ya swappeable para test. | No |
| `StreamPlayer` | Decodifica y reproduce el track actual; anuncia cambios de estado de reproducción vía callback directo (no `EventLoop`) a `ConnectStateHandler::onPlayerStateUpdate()`. | Sí |
| `SpClient` (interfaz) / implementación default | Mecánica HTTP de los endpoints de Spotify (`putConnectState(Raw)`, `putInactive`, resolución de contexto, etc.). | No |
| `ApClient` / `ApConnection` | Conexión binaria al Access Point (Shannon cipher), incluye ahora la sincronización de reloj vía el paquete `Ping`. | No (corre sobre el `socketPoll` compartido) |
| `DealerClient` | WebSocket del dealer: conecta, hace keep-alive, parsea el sobre JSON, postea a `EventLoop`. | Sí |
| `FileProvider` (interfaz) | Resuelve claves de audio / streams de CDN para un track. | — |
| `AuthInfo` / `Authenticator` | Credenciales, identidad del dispositivo, flujo de Zeroconf. | No |

## 3. Mecanismo de despacho de PUT

- PUT de buffering announce (transfer/play): async, siempre vía `runTask()`.
- PUT de cambios de reproducción en curso: async (`condition_variable`,
  wake casi instantáneo).
- Qué protege el mutex compartido: el proto completo `PutStateRequest` +
  `TrackQueueHandler`.
- Latencia mínima de despacho: ~0ms (`putStateCv.notify_one()`) + poll de
  respaldo cada 500ms.
- Consistencia del mecanismo: uniforme — un único emisor (`runTask()`),
  sin caminos sincrónicos alternativos.

La arquitectura actual logra consistencia total en el envío (sin escape
sincrónico) — una decisión tomada deliberadamente esta sesión, no algo
heredado. La latencia de despacho también se resolvió esta sesión,
reemplazando el poll fijo de 50ms original por un `condition_variable`
(wake casi instantáneo + poll de respaldo cada 500ms, ver commit "Replace
the 50ms poll with condition_variable in ConnectStateHandler"). **Queda
pendiente** ampliar la granularidad del lock (Opción 1 del §6, no
implementada) — esa sí sigue siendo una limitación real.

### 3.1 Por qué no alcanzaba con solo acotar el lock

El bloqueo de ~11s que motivó todo el rediseño (`putStateMutex` sostenido
durante el round-trip HTTPS) *en teoría* se podría haber resuelto con un
cambio mucho más chico: que cada handler siguiera mandando su propio PUT
inline, pero soltando el lock antes de la llamada de red en vez de
después.

```cpp
// Alternativa descartada: lock angosto, PUT igual mandado inline por
// cada caller.
bell::Result<> ConnectStateHandler::putState(PutStateReason reason) {
  std::vector<std::byte> encodedBody;
  {
    std::scoped_lock lock(putStateMutex);
    // mutar putStateRequestProto...
    // codificar a encodedBody...
  } // lock liberado ACÁ, antes de la red

  return spClient->putConnectStateRaw(std::move(encodedBody), ...);
}
```

Esto sí resuelve la latencia: con el lock libre durante la red, un hilo
distinto (por ejemplo `StreamPlayer` llamando a `onPlayerStateUpdate()`)
ya no queda esperando detrás de un PUT ajeno.

Lo que esta alternativa **no** resuelve, y que centralizar todo en
`runTask()` sí resuelve sin esfuerzo extra: el **orden de salida de los
PUT hacia el servidor** deja de estar garantizado en cuanto hay más de un
emisor. Con múltiples hilos codificando y mandando cada uno su propio PUT
de forma independiente:

- El orden en que dos PUTs *llegan al servidor* depende del timing de
  TCP/TLS de cada conexión, no del orden en que se mutó/codificó cada uno
  en C++. Un PUT con `message_id` más alto (mutado después) podría llegar
  antes que uno con `message_id` más bajo, mutado primero pero cuya
  conexión tardó más en establecerse.
- El coalescing de 200ms ("last reason wins", no reenviar dentro de la
  ventana) depende de un único punto de decisión "¿ya mandé algo hace
  poco?". Con emisores independientes, dos mutaciones casi simultáneas en
  hilos distintos podrían cada una concluir "no estoy rate-limited" y
  disparar dos PUTs seguidos - para evitarlo haría falta un lock/flag
  *adicional*, separado del de mutación, solo para serializar esa
  decisión - en la práctica, reinventar una versión más pobre de lo que
  ya hace `runTask()` como único emisor.
- Quedaría sin verificar si el cliente HTTP subyacente
  (`bell::http::ConnectionPool`/`DefaultTransport`) es seguro ante
  ejecuciones concurrentes desde hilos distintos - con un único emisor la
  pregunta ni se plantea.

En resumen: acotar el lock arregla la *latencia* (el síntoma observado),
pero no la *consistencia del orden de envío* - esa garantía es la que se
obtiene gratis al centralizar en un único emisor, sin tener que razonar
por separado sobre sincronización de red además de sincronización de
memoria.

## 4. Por qué existe `PlayerStateUpdate` (y por qué no lo sacamos)

go-librespot no tiene un DTO equivalente: `AppPlayer` es una sola clase, y
todo lo que hoy nosotros repartimos entre `StreamPlayer` (decodifica/
reproduce) y `ConnectStateHandler` (interpreta comandos, es dueño del
`PlayerState` de connect-state) vive junto ahí. Cuando `loadCurrentTrack()`
sabe "todavía estoy bufferizando" o "ya tengo un `playbackId`", **es la
misma función que después escribe directo en `p.state.player`** — no hay
frontera que cruzar, así que no hace falta un objeto intermedio: no tocar
un campo alcanza para dejarlo como estaba (la base del punto 3 de esta
sesión: por qué `playbackId` puede ser `nullopt` sin problema ahí).

En esta rama, `StreamPlayer` y `ConnectStateHandler` son dos clases con
hilo propio, separadas a propósito: `StreamPlayer` hace CDN fetch/TLS/
decode Vorbis (un dominio de I/O pesado y propenso a bloquear), y
`ConnectStateHandler` es dueño exclusivo de `putStateRequestProto` bajo su
propio `putStateMutex`. `StreamPlayer` no conoce ese objeto ni ese lock —
solo conoce un `std::function` (`PlayerStateAnnounceCallback`) con una
firma angosta. `PlayerStateUpdate` es el mensaje que cruza esa frontera:
paso de mensajes normal entre dos actores en hilos distintos, no una
torpeza de diseño.

**Evaluamos explícitamente la alternativa de un único objeto compartido**
(replicar el modelo de go-librespot) y la descartamos por dos razones
concretas:

1. **Se disuelve la encapsulación del schema.** Hoy todo el conocimiento de
   qué significa cada campo de `PlayerState` (que `isPlaying` sea siempre
   `true`, la fórmula de `computePlaybackSpeed()`, cuándo tocar
   `playbackId`) vive en un solo lugar, `ConnectStateHandler.cpp`. Compartir
   el objeto crudo esparce ese conocimiento a `StreamPlayer` también —
   exactamente lo opuesto a la recomendación de la sección 5 (achicar la
   superficie de `ConnectStateHandler`, no ensancharla hacia otra clase).
2. **Reintroduce el acoplamiento de locks que este mismo rediseño eliminó.**
   `StreamPlayer::taskLoop()` corre `audioDecoder->processPacket()`
   *deliberadamente* fuera de `playbackMutex` porque puede bloquear en
   HTTP/CDN (comentario propio en el código). Si mutar `PlayerState`
   exigiera tomar el mutex de connect-state desde el hilo de `StreamPlayer`,
   ese mutex compartido podría terminar bloqueando indirectamente el
   `runTask()` de `ConnectStateHandler` — el mismo problema (un hilo lento
   pisándole el envío del PUT a otro) que pasamos toda la sesión
   eliminando, solo que ahora entre dos clases en vez de dentro de una.

La simplicidad del modelo de go-librespot no viene de "un solo objeto" en
sí — viene de no separar audio-decode y connect-state en clases/hilos
distintos. Esa separación es deliberada de este lado (stacks
independientes, cada hilo dimensionado para su propio tipo de I/O) y
ninguno de los bugs encontrados esta sesión sugirió que estuviera mal.
`PlayerStateUpdate` (con su `std::optional<std::string> playbackId`,
ver punto 3) es el costo correcto de mantenerla.

## 5. Áreas de mejora en esta rama (principios SOLID)

### SRP — `ConnectStateHandler` es un god object

Hoy concentra, en una sola clase: interpretación de comandos (7 handlers),
dueño del proto completo, scheduler de rate-limiting, codificación +
envío de red, e incluso es dueño de una `TrackQueueHandler` propia
(construida internamente, ver DIP abajo). Es *por eso* que terminamos con
un único `putStateMutex` cubriendo cuerpos de función enteros: al estar
todo en una clase, todo cae bajo el mismo lock por comodidad, no por
necesidad real. Separar interpretación / estado / scheduling-envío en
clases distintas (ver Opción 2 del §6) haría que la sección crítica async
terminara siendo naturalmente angosta, en vez de cubrir el proto completo
— el estado "de verdad" viviría en otro lado, y el handoff entre clases
sería un struct chico en vez de un lock ancho.

### DIP — `TrackQueueHandler` construido internamente, no inyectado — hecho

`ConnectStateHandler`'s constructor llamaba `createDefaultTrackQueueHandler(...)`
él mismo, en vez de recibir la interfaz ya construida desde `Session`. Ahora
`Session::Session()` lo construye (mismo momento en que ya arma `fileProvider`/
`audioDecoder` para `StreamPlayer`, `spClient`/`eventLoop` ya disponibles) y lo
mueve al constructor de `ConnectStateHandler` como `std::unique_ptr<TrackQueueHandler>`,
guardado por lista de inicialización como el resto de las dependencias. Ningún test
construye `ConnectStateHandler` directamente (no está ni en el build de `test/`),
así que no hubo otro call site que actualizar. Esto habilita mockear
`TrackQueueHandler` el día que se agregue esa clase al test suite — hoy no hay
beneficio medible, es una precondición. No resuelve el God Object en sí (ver
Opción 2 abajo, que sigue pendiente para la parte de separar orquestador/dispatcher).

### Puntos ya bien resueltos (no tocar)

- `SpClient`/`AudioDecoder`/`FileProvider`/`TrackQueueHandler` ya son
  interfaces abstractas con una implementación default + fábrica
  (`createDefaultXxx()`) — ya es SOLID-friendly y no necesita cambios.
- Delegar la mecánica HTTP cruda (reintentos, host, etc.) a `SpClient` en
  vez de reimplementarla en `ConnectStateHandler` ya es correcto.

### OCP — mención menor

`handlePlayerCommand()` es una cadena `if/else` por endpoint. `seek_to`,
`set_queue` y `add_to_queue` ya están implementados (con ~13 comandos
total la cadena sigue siendo manejable). No es prioritario todavía.

## 6. Plan de trabajo (mayor a menor prioridad)

### Opción 1 — Separar `trackQueueMutex` de `putStateMutex` (prioridad media)

El motivo real de sostener `putStateMutex` durante
`trackQueueHandler->loadContext()` no es proteger el proto (nadie más lo
toca fuera del hilo de `EventLoop`) — es proteger `TrackQueueHandler` de
una lectura concurrente de `runTask()`/`encodeProtoTracks()` (que lee
`nextTracks()`/`previousTracks()` desde el otro hilo, para el callback de
nanopb). Separar en dos locks deja `putStateMutex` acotado solo a
mutaciones del proto (todas rápidas, en memoria).

- **Pros**: `loadContext()` (red) deja de bloquear a `runTask()` de mandar
  un PUT pendiente de OTRO handler mientras tanto.
- **Contras**: dos locks introducen riesgo real de deadlock si algún path
  futuro necesita ambos a la vez — requiere definir y documentar un orden
  fijo de adquisición desde el día uno. Sin evidencia hoy de que
  `loadContext()` haya causado un síntoma real (es una mejora
  especulativa, no una corrección de un bug observado) — no es urgente.

### Opción 2 — Reducir la superficie de `ConnectStateHandler` (prioridad media-baja)

Separar en algo como: `ConnectStateHandler` (orquestador liviano, solo
interpretación de comandos) + un dispatcher dedicado (dueño exclusivo de
scheduling/encode/send) + inyectar `TrackQueueHandler` desde `Session` en
vez de construirlo internamente.

**La parte de inyectar `TrackQueueHandler` ya está hecha** (ver DIP arriba) —
era la mitad chica y mecánica de esta opción. Queda pendiente la mitad
grande: separar orquestador de dispatcher.

- **Pros**: habilita la Opción 1 de forma más natural (cada pieza con su
  propio lock, sin coordinarlos a mano dentro de una clase gigante);
  mejora testabilidad (se podría mockear el dispatcher en tests unitarios
  sin tocar producción).
- **Contras**: refactor más grande de lo que parece a primera vista — no es
  mover funciones a otro archivo. Hoy los ~13 handlers de comando mutan
  `putStateRequestProto` campo por campo, directo, bajo el mismo lock que
  después lee `runTask()` — no existe un handoff angosto entre
  interpretación y envío. Separar de verdad exige rediseñar ese handoff
  (¿un struct de PODs? ¿setters expuestos por el dispatcher?) antes de
  poder mover código - una decisión de diseño nueva, no un refactor
  mecánico. Toca firmas y constructores, mayor superficie de regresión —
  hay que reprobar todo el flujo de comandos en hardware real. No resuelve
  ningún bug conocido hoy: es deuda técnica preventiva, ROI inmediato bajo
  comparado a la Opción 1. Evaluado explícitamente y descartado por ahora
  (sesión 2026-08-09) - no encarar sin que el usuario lo pida de nuevo.

**Diseño concreto (evaluado sesión 2026-08-10, no implementado)**: mirando
los helpers `...Locked()` que ya salieron de una ronda de deduplicación
anterior (`refreshTrackAndIndexLocked`, `announcePlaybackFlagsLocked`,
`applySeekLocked`, `applyPlayerOptionsLocked`, `applyRepeatContextLocked`,
`currentPositionMsLocked`), esos ya son setters angostos de facto — el
handoff que falta no es tan lejano como parece. Forma concreta de 3 clases:

- **`ConnectStateModel`** (dueño exclusivo del estado, con el codec
  adentro): dueño exclusivo de `putStateRequestProto` + `putStateMutex` +
  `nextMessageId`/`currentTrackStartedAtMs`/`nextManualQueueId`. Expone
  setters angostos que cubren **todos** los campos que hoy se mutan, no
  solo los 6 ya extraídos — hace falta agregar el equivalente para lo que
  `handleTransferCommandLocked` (`ConnectStateHandler.cpp:635-822`, la más
  grande, 187 líneas) y `handlePlayCommandLocked` (`:823-935`, 112 líneas)
  todavía tocan directo: `deviceInfo`, `isActive`, `context`, `options`.
  También absorbe `prepareAndEncodeLocked()`/`encodeProtoTracks` — el codec
  vive junto al dato que serializa, self-locking (no asume lock externo, a
  diferencia de hoy).
- **`ConnectStatePutSender`** (scheduling y envío puro, separado del
  estado): el `bell::Task`, `runTask()`/`wakeTask()`, y el estado de
  *scheduling puro* — `putStateCv`, `lastPutStateTime`, `putStatePending`,
  `putStateDueTime`, `pendingPutStateReason`. Nunca toca el proto directo,
  solo llama `model->prepareAndEncode(reason, outBody)` y después
  `spClient->putConnectStateRaw(...)`. `schedule(reason)` es su única API
  de entrada.
- **`ConnectStateHandler`** (orquestador, lo que queda): interpretación de
  JSON de los ~13 comandos + `handleClusterUpdate`/`handleSetVolume`/
  `putInactive` (códecs de push, protobufs distintos a `PutStateRequest`) +
  los 6 `request*()`. Cada handler pasa a ser: parsear JSON → decidir (a
  veces con `trackQueueHandler`) → llamar setters de `model` →
  `sender->schedule(reason)`. Nunca toca `putStateRequestProto` ni el mutex
  directo.

**Matiz que las secciones de arriba no separan del todo**: esta
reorganización (quién es dueño de qué código) es un eje distinto de la
granularidad del lock (Opción 1). Se puede hacer esta Opción 2 con un solo
mutex — viviendo adentro de `ConnectStateModel`, ni más ni menos angosto
que hoy — sin tocar la latencia de `loadContext()` para nada. Son mejoras
independientes, combinables después, no una prerequisito de la otra: la
Opción 2 por sí sola es una mejora de mantenibilidad pura, no promete nada
de concurrencia.

**Costo**: no es mecánico. Cada uno de los 13 handlers necesita auditoría
— qué campos toca hoy directo vs. ya detrás de un helper — y los dos
grandes (`transfer`/`play`) concentran la mayoría del trabajo real.
`Session.cpp` pasa de construir 1 objeto a 3, con el wiring de
dependencias entre ellos (`timeProvider` queda en el orquestador, que le
pasa valores ya calculados a `model` — el modelo no consulta reloj
directamente). Trabajo de varias sesiones, mayor que la ronda de 2.3 (toca
firmas de constructor, no solo extrae bloques) — exige re-probar en
hardware el flujo completo de comandos. Sigue sin haber ningún bug real
que lo empuje — mismo veredicto que ya estaba arriba, ahora con forma
concreta en vez de abstracta. **No implementar sin que el usuario lo pida
explícitamente.**

**Confirmado dos veces, no una**: además de la evaluación de sesión
2026-08-09, se repitió el análisis con ojos frescos ese mismo día (sin
apoyarse en la conclusión anterior) y llegó al mismo veredicto — nada
cambió respecto a por qué el handoff angosto no existe hoy. No revisitar
sin una razón concreta nueva (bug real, o que el número de comandos/codecs
crezca sustancialmente).

**Candidato menor descartado, no perseguido**: durante esa segunda
revisión apareció un patrón de decode repetido (`base64Decode` → log de
error → `nanopb_helper::decodeFromVector` → log de error, ~10-13 líneas
c/u) en `handleClusterUpdate`/`handleSetVolume`/el arranque de
`handleTransferCommandLocked` — candidato a un helper `decodePayload<T>()`,
mismo espíritu que `hexDump`/`manualQueueUid`. Impacto marginal (2-3
sitios) y no ataca el God Object en sí (boilerplate de codec, no reduce
responsabilidades). Ofrecido, el usuario cerró el tema sin pedirlo — no
implementado.

### Opción 2b — Split físico a `connect/`, sin tocar la clase (alternativa liviana a la Opción 2)

Evaluada junto con la Opción 2 (sesión 2026-08-10) como estrategia
alternativa de menor riesgo: mover `ConnectStateHandler.cpp`/`.h` a una
carpeta dedicada, partiendo además el `.cpp` en 3 archivos por rol
(scheduling/envío, interpretación de comandos, códecs de push) — sin
cambiar la clase, el lock, ni ninguna firma.

**La mitad de la relocación ya está hecha**, aunque no por esta
evaluación: el commit `20016b05` ("Group Spotify Connect files into
src/connect and include/connect", 2026-08-11) movió
`ConnectStateHandler.cpp`/`.h` — junto con `ConnectReceiver`,
`ZeroconfServer` y `PlaybackNotifications.h` — a
`main/src/connect/`/`main/include/connect/`, actualizando el
`file(GLOB ...)` de `main/CMakeLists.txt` y todos los include sites. Fue
una limpieza de organización de carpetas (agrupar todo lo de Spotify
Connect), no una respuesta a esta opción — pero cubre el gotcha de CMake
que este documento señalaba.

**Lo que sigue sin hacer**: partir el `.cpp` (hoy ~1350 líneas, un solo
archivo dentro de `src/connect/`) en 3 archivos por rol. Eso seguiría sin
resolver el SRP real — sigue siendo una sola clase con un solo lock
ancho, solo que repartida en más archivos — y los helpers de namespace
anónimo (`hexDump`, `manualQueueUid`, etc.) necesitarían un header interno
compartido si se usan desde más de un `.cpp` nuevo.

Descartada por ahora junto con la Opción 2 completa — se documentó solo
el diseño de 3 clases (Opción 2 arriba). Queda acá como alternativa de
menor esfuerzo si algún día se quiere solo mejorar navegabilidad sin
encarar el rediseño completo del handoff.

### Opción 3 — Tabla de despacho para comandos — won't fix (descartado)

Se implementó (reemplazando la cadena `if/else` de `handlePlayerCommand()`
por un `std::unordered_map<std::string_view, CommandHandlerFn>`, con 8 de
los 13 comandos necesitando un adaptador privado de una línea solo para
unificar la firma a `bell::Result<> (ConnectStateHandler::*)(const
tao::json::value&)`) y compiló limpio (`cspot_cli`), pero el usuario la
juzgó sobre-ingeniería y pidió descartarla — revertida sin commitear.
Confirma el propio "contra" que este documento ya anotaba más abajo: con
~13 comandos la tabla no paga su propio costo, sobre todo por el ruido de
los adaptadores necesarios para unificar firmas heterogéneas
(`transfer`/`pause`/`resume`/etc. no tenían la misma firma que
`skip_next`/`play`/`seek_to`/...). Si en el futuro el número de comandos
crece sustancialmente y ese ruido dejara de dominar el diff, recién ahí
vale la pena revisitarlo — no antes.

### Opción 4 — Config struct inyectado, en vez de valores hardcodeados (prioridad media, alcance amplio)

A diferencia de las opciones 1-3 (todas acotadas a `ConnectStateHandler`),
esta cruza varias clases: device name, formato de audio
(`AudioFormat_OGG_VORBIS_160` fijo en `FileProvider.cpp`), `chunkSize`
(32KB en `CDNDataStream.cpp`), el umbral de descarte de conexión lenta
(2000ms, mismo archivo), y otros valores hoy sueltos como constantes
locales o hardcodeados inline. Hoy no existe un patrón de config
centralizado — cada clase lee su propia constante.

- **Pros**: centraliza valores hoy dispersos y duplicados en su
  significado (ej. el chunk size afecta directamente el margen de buffer
  del que dependen tanto el mecanismo de retry como el de descarte de
  conexión lenta - hoy ese vínculo es implícito); habilita cambiarlos sin
  recompilar cada clase por separado.
- **Contras**: toca constructores y call sites en varias clases
  (`FileProvider`, `CDNDataStream`, `ConnectStateHandler`, `Session`) -
  no es un cambio de una sola función. Mejor hacerlo incremental,
  arrancando por 2-3 valores de alto valor (calidad de audio, chunk
  size) en vez de una pasada grande sobre todo lo hardcodeado de una
  vez.

## 7. Precauciones al aplicar estos cambios

- **Preservar el guard de overflow de `hasEverSentPutState`**: sin este
  flag, la primera llamada a `putState()` en la vida del objeto confiaba en
  `now - lastPutStateTime >= kStatePutMinInterval` para decidir "no está
  rate-limited todavía" (`lastPutStateTime` arranca en
  `steady_clock::time_point::min()`, pensado para que esa resta dé un
  valor enorme la primera vez). Esa resta es en realidad un overflow con
  signo (`min()` es `duration::min()`, ~`INT64_MIN`; `now()` es un valor
  chico y positivo - la resta no entra en `int64_t` de ninguna manera).
  Con `-fsanitize=signed-integer-overflow` esto directamente crashea; en
  un build sin sanitizer (todos los builds reales acá) se envuelve
  silenciosamente a una duración enorme y NEGATIVA - `>= kStatePutMinInterval`
  da `false` en esa primera llamada, siempre, en cada arranque: el primer
  `putState()` de la vida del objeto terminaba siempre yendo por el camino
  diferido de `taskLoop()` en vez de mandarse en el momento, exponiéndolo
  de forma determinística a una carrera contra lo que sea que corriera
  después en el hilo de `EventLoop` (confirmado en hardware real: un
  comando de transfer llegando ~1s después del boot ganaba esa carrera
  contra este mismo PUT diferido). `hasEverSentPutState` evita la
  aritmética de tiempos por completo en esa primera llamada, en vez de
  buscar otro `time_point` centinela que no haga overflow. Cualquier
  reescritura del scheduling tiene que mantener ese short-circuit
  explícito, no asumir que la aritmética de tiempos es segura.
- **Orden de adquisición de locks fijo, si se separan** (Opción 1):
  documentar explícitamente (comentario en el header) cuál lock se toma
  primero si algún path llega a necesitar ambos, para que un cambio futuro
  no introduzca un deadlock por inversión de orden.
- **`EventLoop::post()` no debe bloquear** mientras se sostiene cualquiera
  de estos locks — confirmar que sigue siendo así si se reestructura el
  locking, ya que varios handlers llaman `eventLoop->post(...)` dentro de
  la sección crítica actual.
- **Probar en hardware real antes de dar por cerrado cualquiera de estos
  cambios** — son cambios de concurrencia, difíciles de validar solo
  leyendo código o con el build de CLI. Seguir el flujo habitual: build +
  flash + monitor en el JC3248W535, revisando en particular transfer,
  pause/resume repetidos rápido, y skip_next/prev en sucesión (los casos
  que más estresan el scheduling del PUT).
- **Ninguna de estas opciones es prerequisito de otra**: la 1 y la 2 están
  relacionadas entre sí (la 2 facilita la 1, pero la 1 no requiere la 2).
  La 4 es independiente de las tres primeras (alcance distinto, no toca
  locking ni la superficie de `ConnectStateHandler`).
