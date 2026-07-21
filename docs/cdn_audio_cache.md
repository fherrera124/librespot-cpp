# Cache de audio del CDN

**Estado: diseño, no implementado.** Este documento registra la
investigación y las decisiones de diseño tomadas en conversación antes
de escribir código — sirve como punto de partida para la
implementación real cuando se arranque, y como historial de por qué se
descartaron las alternativas. Formato similar a
`dealer_websocket_migration.md`: organizado por tema, no
cronológicamente, con una sección final de iteraciones/observaciones
en orden.

---

## 1. Motivación

Evitar volver a bajar del CDN de Spotify un track ya reproducido
(repetición, loop de una canción, "atrás" en la cola) guardando una
copia local. No es un objetivo de "modo offline" ni de cachear una
biblioteca completa — el espacio disponible (ver §4) acota esto a un
puñado de tracks recientes.

## 2. Dónde engancha en el pipeline existente

`CDNAudioFile.cpp` (`https://github.com/fherrera124/librespot-cpp/blob/master/src/CDNAudioFile.cpp`) hace
**fetch + decrypt intercalado por range request**, no descarga todo y
desencripta después:

- `openStream()` (líneas 49-115) prefetchea un header de 8KB
  (`OPUS_HEADER_SIZE`) y un footer de ~12KB
  (`OPUS_FOOTER_PREFFERED`), ambos cacheados en vectores miembro.
- `readBytes()` (líneas 117-230) sirve desde esos buffers de
  header/footer cuando el rango pedido cae ahí, o dispara un nuevo GET
  con rango vía `fetchRange()` (tamaño = bitrate × 2.4s,
  `HTTP_BUFFER_SECONDS`, línea 81) y lee el body de la respuesta
  directo a `httpBuffer` (líneas 174-215).
- **Desencriptado in-place inmediatamente después de cada lectura
  HTTP** (AES-CTR, `Crypto::aesCTRXcrypt`, llamado en líneas 84, 104,
  216; método `decrypt()` en líneas 236-240; IV por chunk vía
  `bigNumAdd(audioAESIV, pos/16)`).

**Punto clave**: lo que sale de `CDNAudioFile` hacia el decoder
(Vorbis/MP3) es contenido **ya desencriptado pero todavía
comprimido** (el contenedor Ogg/MP3 original) — no PCM. La capa de
cache tiene que vivir acá, no más abajo en el pipeline: cachear PCM
sería 5-10x más grande por track, inviable en el presupuesto de
espacio de §4.

## 3. Qué se cachea y con qué key

**Se cachea el contenido post-`decrypt()`, no el ciphertext.**
Cachear ciphertext + la key por separado no ahorra nada extra (el
decrypt es barato, AES-CTR) y suma el problema de persistir la audio
key en flash. Cachear ya-desencriptado además evita, en un hit, pedir
la key de nuevo — ver §3.1.

**Key del cache: `fileId`**, no `trackId`. Ya existe en
`TrackQueue.cpp` (~línea 290, `bytesToHexString(fileId)`), es
específico por formato/bitrate — la granularidad correcta, porque el
mismo track en otra calidad es, para el CDN, otro archivo. (El
`trackHash` de `CentralAudioBuffer::AudioChunk` — ver
`docs/aprendizaje.md`, entrada 2026-07-17 — es un concepto no
relacionado y de código muerto, no sirve para esto.)

### 3.1. Interacción con la audio key

La key se pide hoy vía `MercurySession::requestAudioKey(trackId,
fileId, callback)` (`MercurySession.cpp:410`), enviando
`[FILEID][TRACKID][seq][0x00,0x00]` sobre la conexión AP ya
Shannon-encriptada. Es transitoria, solo en memoria
(`TrackQueue.cpp:299-309`), pedida de nuevo en cada
`stepLoadAudioFile()`. **Un hit de cache salta este round-trip
completo** (no hace falta la key si el contenido ya está
desencriptado) — no es solo ahorro de ancho de banda del CDN, también
un round-trip menos de Mercury/AP.

## 4. Presupuesto de espacio y hardware

Board real: **Guition JC3248W535** (ESP32-S3 + display QSPI
AXS15231B). 16MB de flash (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`),
`partitions.csv` reserva 4MB para la partición `factory` (subido de
1900K en F61 para la UI) — quedan **~12MB sin particionar**. 8MB de
PSRAM octal SPI, ya comprometida para buffers de display, DMA, TLS,
Wi-Fi/lwIP — no hay margen ahí para cache de audio. Hoy no hay ningún
filesystem montado (sin SPIFFS/LittleFS/FATFS en uso en el código
actual).

**Corrección del usuario (2026-07-17)**: el board sí trae slot de
tarjeta SD — el driver todavía no está implementado. Esto cambia el
análisis de espacio: con SD, la restricción de "unos pocos tracks"
por flash interna deja de aplicar.

Con solo flash interna (~12MB útiles) y ~3-5MB por track comprimido,
el espacio da para **2-4 tracks** cacheados — de ahí que el objetivo
en §1 sea "no repetir fetch", no "cachear una biblioteca".

## 5. Opciones de almacenamiento evaluadas

### 5.1. Slots en flash interna (raw partition + índice chico)

Partición nueva sobre los ~12MB libres, N slots fijos de tamaño
acotado, índice `fileId → {slot, tamaño, completo}`, LRU simple entre
slots. Sin overhead de filesystem generalista — encaja con el patrón
de acceso real (pocos blobs grandes, escritura secuencial, casi sin
archivos chicos). Contra: código propio a mantener; capacidad muy
limitada (§4).

### 5.2. LittleFS sobre una partición nueva

Componente estándar de ESP-IDF (`esp_littlefs`), wear-leveling real,
API de archivos normal. Sensato si esa partición fuera a usarse para
más cosas después (logs, config); para este caso puntual es overhead
de filesystem completo para un workload que no lo necesita.

### 5.3. SD card (pendiente: driver no implementado)

Una vez montada (FAT32 vía `esp_vfs_fat_sdspi_mount`/`sdmmc`), mismo
patrón que 5.1/5.2 pero sin la restricción de espacio — capacidad
reduce a "cuánta SD pongas". Dispositivo queda autónomo, sin depender
de que otra cosa esté prendida en la red. Costo: terminar el bring-up
del driver SD, que hoy no existe en el proyecto.

### 5.4. Cache server remoto (LAN) vía HTTP

**Decisión: HTTP, no un protocolo de socket a más bajo nivel.**
Motivo principal: `CDNAudioFile` ya habla range requests contra
`HTTPClient`/`SocketStream` — un servidor de cache HTTP con soporte de
`Range` hace que el **hit path sea la misma `CDNAudioFile` que ya
existe, apuntada a otro host**. No hace falta reescribir la lógica de
lectura/seek (prefetch de header/footer, `HTTP_BUFFER_SECONDS`, etc.)
para un segundo transporte. El overhead de headers HTTP es
insignificante en LAN comparado con la latencia real del CDN de
Spotify por internet.

**Un GET a un `fileId` inexistente devuelve `404` — esa es la señal
de miss.** No hace falta un endpoint de "check" separado antes de
pedir el contenido: una sola request resuelve hit-o-miss y, si es hit,
ya trae el body.

Servidor: puede ser tan simple como un servidor HTTP estático con
soporte de `Range` (nginx, Caddy, `python -m http.server`) para servir
hits — no necesita ser parte de este codebase ni estar en C++. El lado
de escritura (recibir y guardar bytes en un miss) sí necesita algo más
que un file server estático puro (ver §6.2).

Contras frente a SD: dependencia nueva — el ESP32 queda atado a que
ese otro dispositivo esté vivo y alcanzable en la red, un modo de
falla que SD no tiene.

**Ambas (SD y cache remoto) se consideran viables y no mutuamente
excluyentes** — el usuario mostró interés en las dos.

## 6. Diseño de flujo (propuesto por el usuario, validado)

> Consultar al cache server; si lo encuentra (hit), mandar al sink lo
> que vaya devolviendo ese server; si es miss, ir descargando del CDN
> de Spotify como ahora, y en paralelo guardar en el cache server.

### 6.1. Hit path

`TrackQueue.cpp`, antes de `stepLoadAudioFile()` (que hoy pide la
audio key vía Mercury incondicionalmente y arma la `CDNAudioFile` real
sin chequear nada): intentar un GET con rango a
`http://<cache-server>/<fileId>`. Si responde con contenido (200):
usar ese host como fuente para el `CDNAudioFile` existente — sin
código nuevo de "stream al sink", es el mismo camino que ya corre hoy,
apuntado a otro origen. Si 404: seguir el camino actual (audio key +
CDN real) y lanzar el path de escritura de 6.2 en paralelo.

### 6.2. Miss path — escritura al cache (pendiente de diseñar en detalle)

**No puede ser síncrono con la reproducción.** Necesita un task
separado — mismo patrón que `i2sFeedTask` de `BufferedAudioSink`
(`bell/main/audio-sinks/include/esp/BufferedAudioSink.h`), que ya
desacopla el fetch de red del feed a I2S vía un ring buffer propio.
El writer del cache recibiría los chunks decrypted-pero-comprimidos
que `CDNAudioFile::readBytes()` ya produce, por su propia
cola/ring buffer, y los subiría al cache server sin bloquear el audio
si el server está lento o caído.

**Completitud**: un archivo de cache parcial (usuario hace skip a
mitad de track, se cae la red) no puede quedar servible como hit más
adelante — si no, un futuro hit reproduciría un archivo cortado y el
playback fallaría cerca del final. Dos opciones, sin decidir todavía:

- Escribir a un nombre temporal y hacer rename atómico al terminar.
- El servidor trackea bytes-recibidos vs `Content-Length` esperado, y
  solo responde 200 (hit) cuando están completos — mientras tanto,
  404 o algún código que indique "en progreso, no servible aún".

**Protocolo de escritura**: todavía no definido en detalle. HTTP PUT
es la opción natural para seguir aprovechando la reutilización de
`HTTPClient`, pero un file server estático puro (nginx/Caddy) no
recibe uploads sin configuración adicional — el cache server
probablemente necesita ser una app chica propia (no
necesariamente en C++/parte de este repo — puede vivir en cualquier
lenguaje/plataforma) que entienda: GET con `Range` → servir o 404;
PUT/POST por `fileId` → recibir y persistir bytes, más la señal de
completitud de arriba.

## 7. Preguntas abiertas

- ¿SD y cache remoto conviven (ej. SD como primario, remoto como
  fallback), o se implementa uno primero y se evalúa el otro después?
- Si SD tarda en estar lista, ¿vale la pena implementar 5.1 (slots en
  flash) como interino, sabiendo que es descartable una vez haya SD?
- Política de evicción concreta para 5.1 si se implementa (LRU por
  slot es la propuesta, sin detallar aún).
- Autenticación/seguridad del cache server remoto — probablemente
  innecesaria por ser LAN-local y contenido no especialmente sensible,
  pero no evaluado en profundidad todavía.
- Confirmar contra la lógica real de selección de `fileId` en
  `TrackQueue.cpp` (líneas ~243-290) que un cambio de calidad de audio
  en runtime no genera colisiones de key en el cache (debería estar
  cubierto porque `fileId` ya es por formato/bitrate, pero no
  verificado línea por línea todavía).

## 8. Iteraciones / observaciones (orden cronológico)

- **2026-07-17**: primera propuesta del usuario (cachear audio
  encriptado mientras se sigue escuchando, con lógica de "está en
  cache" antes de decidir fuente). Investigación del pipeline real
  (`CDNAudioFile.cpp`) llevó a la decisión de cachear post-decrypt en
  vez de ciphertext (§3). Investigación de hardware (BSP
  `bsp_jc3248w535`, `partitions.csv`, `sdkconfig`) no encontró SD
  card ni filesystem montado — **corregido por el usuario**: el board
  sí trae slot SD, driver aún no implementado (§4). Se evaluaron 4
  opciones de almacenamiento (§5); HTTP elegido sobre protocolo de
  socket custom para el cache remoto por la reutilización directa de
  `CDNAudioFile`/`HTTPClient` (§5.4). Usuario validó el flujo
  hit/miss propuesto (§6); pendiente detallar el protocolo de
  escritura del miss path (§6.2) y arrancar implementación.