# Suite de tests de host — `components/tests/`

Este documento reúne todo lo relacionado a la suite de tests que corre
en host (Linux, fuera de `idf.py build`), separada de
`spotify_component_analysis.md` (que sigue siendo el documento de
hallazgos/bugs del motor `cspot`/`bell` en general). Los hallazgos de
acá **siguen numerados dentro de la misma secuencia global de
F-numbers** (F101, F102...) — se movieron de documento por alcance
(igual que ya pasa entre `spotify_component_analysis.md` y
`app_arquitectura.md`), no se renumeraron. Cualquier referencia en el
código (`// ver F101`, `// see docs/spotify_component_analysis.md,
finding F101`, etc.) que todavía apunte al documento viejo debería
apuntar acá.

**Qué vive en `components/tests/`**: un proyecto CMake
standalone (no es un componente de ESP-IDF, `idf_component_register()`
nunca se llama ahí) que compila el código real y sin modificar del
motor para host, y lo corre bajo `doctest` (unit tests) y
ThreadSanitizer (tests Tier 2 de concurrencia/protocolo). Requiere
`libmbedtls-dev` (paquete de sistema) y que el build real de ESP32
(`idf.py build`) haya corrido al menos una vez (reusa los `.pb.c`/`.pb.h`
generados ahí). Ver la entrada F101 más abajo para la arquitectura
completa (`FakeAP`, `FakeCdnServer`, el stub `ApResolveStub`, por qué
algunos hallazgos quedaron fuera de alcance para testear en host).

**Cuarto binario — `ws_transport_echo_test`** (2026-07-14, parte de la
migración a Dealer, no un F-number propio — ver
`docs/dealer_websocket_migration.md`, Fase 4a, para el contexto y los
resultados completos): valida la implementación RFC 6455 de
`WebSocketTransport` (`src/WebSocketTransport.cpp`, única para
firmware y host — ver §1 de ese documento), a través de su rama
host-only adicional de `ws://` en texto plano (sin TLS, solo para
esta prueba local), contra un servidor independiente y estricto —
`ws_echo_server.py`, sobre la librería python `websockets`
(requiere `python3 -m pip install websockets` o equivalente). Cubre
handshake, masking, longitudes de payload de 7/16/64 bits en ambas
direcciones, ping/pong de protocolo (el servidor desconecta a quien no
responde) y close handshake. Correr:

```bash
python3 ws_echo_server.py 18765 &      # desde tests/
./build/ws_transport_echo_test         # exit 0 = ALL PASS
```

---

### F101 — Test Tier 2 de F93: sesión real de `cspot` contra un AP falso, en host, bajo ThreadSanitizer (Hardening/testing, aplicado)

F93 se confirmó en hardware real (logs limpios, reconexiones sanas sin
cortar audio), pero el usuario pidió ir un paso más allá: un test
automático y repetible que ejercite el mutex de F93 bajo concurrencia
real, sin depender de dejar el hardware corriendo toda una noche ante
cada cambio futuro ("no es viable correr el hw toda una noche ante
cada cambio crítico"). Se evaluaron dos niveles — Tier 1 (patrón de
locking aislado, sin código real) y Tier 2 (código de producción real,
compilado para host, contra un servidor AP falso) — y se eligió Tier 2
explícitamente por el usuario, más costoso de construir pero
verificando el código real, no una reimplementación aparte del patrón
de locking.

**Qué se construyó** (`components/tests/`, fuera de
`idf_component_register()` — `idf.py build` nunca la toca):

- `fake_ap.{h,cpp}`: un "AP falso" mínimo que habla lo justo del
  handshake de login real de Spotify — Diffie-Hellman (F98/F100) +
  desafío HMAC-SHA1 de 5 rondas, replicando exactamente
  `AuthChallenges::solveApHello()` del lado servidor (con las claves
  Shannon intercambiadas) — para que el `Session`/`MercurySession`
  real, sin ningún cambio, complete `connect()` +`authenticate()`
  contra él y quede autenticado con un `APWelcome` de mentira.
- `ApResolveStub.cpp/.h`: reemplaza `ApResolve.cpp` (que
  `Session::connectWithRandomAp()` — el path que usa
  `MercurySession::reconnect()` — llama incondicionalmente) para que
  apunte siempre al mismo AP falso en vez de hacer un GET HTTPS real a
  `apresolve.spotify.com`. `ApResolve.cpp` real no se usa simplemente
  porque no hace falta para lo que este test ejercita — nada que ver
  con portabilidad de `TLSSocket` (ver nota más abajo: ese bloqueador
  ya se resolvió y `unit_tests` sí usa `TLSSocket.cpp` real, F85/F86).
- `f93_concurrency_test.cpp`: arma una `MercurySession` real, conecta
  contra el AP falso, autentica, arranca `startTask()`, y lanza: 6
  hilos "atacantes" llamando `executeSubscription`/`execute`/
  `requestAudioKey`/`unregisterAudioKey`/`getCountryCode` en bucle, un
  hilo "dispatcher" bombeando `handlePacket()`, y un hilo de "caos de
  red" que mata la conexión activa cada 200-800ms para forzar
  `reconnect()` real, repetidamente, durante toda la duración del test.
- `CMakeLists.txt`: build standalone, plano, de la subrama de
  `bell`/`cspot` realmente necesaria (no usa `add_subdirectory(bell)`
  de `CMakeLists.txt (repo root)`, que arrastraría todo `bell` — codecs,
  sinks de audio, etc. — en vez lista a mano los `.cpp` concretos, más
  los `.pb.c` ya generados por el build de ESP32 real en
  `build/esp-idf/librespot_cpp/protobuf/`, reutilizados tal cual).
  Requiere `libmbedtls-dev` del sistema
  (probado con 3.6.2 de Ubuntu, headers/libs reales de mbedTLS con PSA
  Crypto — no ESP-IDF) y usa el path Linux **nativo** que `bell/
  CMakeLists.txt` ya traía (`find_package(MbedTLS)`, con su propio
  `cmake/FindMbedTLS.cmake`) — no `idf.py --preview set-target linux`,
  que se consideró primero pero resultó innecesariamente pesado una
  vez confirmado que este camino más simple ya funcionaba.

**Bug real encontrado y corregido, en el código del test, no en
`cspot`/`bell`**: la primera versión de `fake_ap.cpp` construía cada
ronda del desafío HMAC como `[número de ronda] + combined`. El
handshake fallaba siempre en `authenticate()` (timeout de 3s de
`PlainConnection`, tanto del lado que espera leer como del que espera
escribir). Verificado con instrumentación temporal (revertida) en
ambos lados: el secreto DH compartido coincidía byte a byte, el
mensaje `combined` coincidía byte a byte (checksum), pero el HMAC
resultante era distinto — imposible si la entrada es realmente
idéntica. La causa real: `AuthChallenges::solveApHello()` hace
`challengeVector.insert(challengeVector.begin(), data.begin(),
data.end())` sobre un vector que ya tenía un solo byte `[x]` — eso
inserta `data` **antes** del byte existente, empujándolo al final, así
que el orden real en el cable es `combined + [x]`, no `[x] + combined`
como se había asumido al escribir el AP falso. Corregido en
`fake_ap.cpp` para igualar exactamente ese orden. (De paso, una
hipótesis intermedia — que la clave PSA de mbedTLS no fuera segura
bajo concurrencia entre dos hilos del mismo proceso — llevó a rediseñar
el AP falso para correr en un **proceso hijo (`fork()`)** en vez de un
hilo hermano, aislando por completo su estado de mbedTLS/PSA del
proceso cliente. Esa hipótesis resultó no ser la causa real del bug,
pero el diseño por proceso se mantuvo: es arquitectónicamente más
correcto para lo que representa — un AP real sería un proceso/máquina
separado, no memoria compartida — y no tiene ninguna desventaja real.)

**Problema de rendimiento encontrado y corregido en `BigUint::mod()`**
(`Crypto.cpp`, la misma rama no-ESP de F100): el `mod()` original hacía
una iteración por cada bit del dividendo (hasta ~1536 bits, producto de
dos números de 768 bits), y cada iteración asignaba un `BigUint`/
`std::vector` nuevo en el heap vía `shiftLeft1()`/`subtract()` — hasta
~1536 asignaciones por llamada a `mod()`, y `modExp()` llama a `mod()`
~1500+ veces. Medido: ~9.6s para un solo intercambio DH bajo
ThreadSanitizer (que instrumenta cada acceso a memoria), muy por
encima del timeout de socket de 3s que `PlainConnection` tiene
hardcodeado. Reescrito para operar in-place sobre un único buffer de
tamaño fijo (una sola asignación por llamada a `mod()`, no miles) —
bajó a ~1.1-1.4s por `modExp()` bajo TSan, verificado con un programa
de timing standalone. `shiftLeft1`/`subtract`/`compare` quedaron sin
uso tras la reescritura y se eliminaron. Esto **no afecta el firmware
ESP32** (rama `#ifndef ESP_PLATFORM`, el ESP32 sigue usando
`mbedtls_mpi_*` con aceleración por hardware — confirmado con `idf.py
build` limpio, mismo margen de flash).

Aun con esa mejora, el margen contra el timeout de 3s sigue siendo
ajustado bajo TSan (dos `modExp()` en secuencia, uno por lado, más el
resto del handshake) — se observó una falla ocasional del handshake
inicial por esta razón. Como `MercurySession::reconnect()` ya reintenta
para siempre por diseño (comportamiento de producción, no relacionado
con este test), el único punto donde esto importa es el handshake
inicial de arranque del test — se agregó ahí un reintento acotado (5
intentos) en el harness del test (`f93_concurrency_test.cpp`, no en
código de producción).

**Otro bug encontrado en el propio harness**: `killCurrentConnection()`
escribe un byte a un pipe hacia el proceso hijo del AP falso; si el
extremo de lectura del hijo ya estaba cerrado, la disposición por
defecto de `SIGPIPE` mataba **todo el proceso del test**. Corregido con
`signal(SIGPIPE, SIG_IGN)` al inicio de `main()` — patrón estándar para
este tipo de escritura best-effort a una tubería/socket que puede
haberse cerrado del otro lado.

**Verificación real**: múltiples corridas limpias de 20-30s bajo
ThreadSanitizer (`TSAN_OPTIONS=die_after_fork=0`, necesario porque el
AP falso hace `fork()` y sigue trabajando con varios hilos después —
TSan aborta el hijo por defecto si no se desactiva), 0 warnings de
ThreadSanitizer en todas las corridas, con los hilos atacantes
ejecutando cientos de `executeSubscription`/`requestAudioKey`/etc. de
verdad contra la sesión real, entrelazados con reconexiones forzadas
reales — incluyendo corridas que pasaron tanto por
"Reconnection successful" (reconexión completa exitosa) como por
"Cannot reconnect, will retry in 5s" (fallo transitorio, reintentado
correctamente) sin ninguna carrera detectada en ningún caso.

**Nota aparte, resuelta más tarde (2026-07-17)**: al compilar
`bell/main/io/TLSSocket.cpp` para host originalmente se encontraba que
incluía `esp_heap_caps.h` sin ninguna guarda `#ifdef ESP_PLATFORM` — un
bug de portabilidad genuino en una capa de `bell` que se supone
genérica. Este test (`f93_concurrency_test`) nunca lo necesitó (no usa
`HTTPClient`/`TLSSocket`, ver `ApResolveStub.cpp` arriba) así que nunca
se tocó por eso, pero el bloqueador en sí ya no existe: el include
salió junto con `logInternalHeap()` (dejó de hacer falta, F64), y
`unit_tests` (F85/F86, más abajo) ya compila y linkea el `TLSSocket.cpp`
real para host sin cambios.

**Cómo compilarlo y correrlo** (requiere `libmbedtls-dev` instalado):

```
cd components/cspot/tests
cmake -S . -B build   # -DF93_TEST_TSAN=OFF para una corrida rápida sin TSan
cmake --build build -j
TSAN_OPTIONS="die_after_fork=0" ./build/f93_concurrency_test 30  # segundos de duración
```

### F102 — Suite de tests de host más amplia: unit tests (doctest) + un segundo test Tier 2 (F87) + hardening de F34, sobre la base que dejó F101 (Hardening/testing, aplicado)

Con F101 (el test Tier 2 de F93) funcionando, el usuario pidió ampliar a
tres frentes a la vez: más findings críticos con tests Tier 2, una suite
de regresión general para el motor, y unit tests de piezas aisladas.
Se armó todo sobre `components/tests/` (la misma carpeta de
F101, sigue fuera de `idf.py build`).

**Framework elegido**: `doctest` (vendorizado, un solo header,
`tests/external/doctest/doctest.h`, MIT) — decisión explícita del
usuario entre eso y macros propias sin dependencias nuevas. Un solo
header, sin build system propio, no rompe la filosofía de "sin
dependencias externas nuevas para agregar funcionalidad real" del
proyecto (que sigue aplicando a `BigUint`/`cJSON`/etc.) porque esto es
tooling de test, no código que termina en el firmware.

**Cuatro binarios en total ahora** (`CMakeLists.txt` del directorio, con
un `CSPOT_SHARED_SOURCES`/`CSPOT_SHARED_INCLUDES`/`CSPOT_SHARED_DEFINES`
común para evitar duplicar la lista larga de fuentes reales entre
targets):

1. **`unit_tests`** (nuevo, doctest, 24 casos / ~97 asserts, **build
   `Release` sin TSan, ~4.3s**) — ver detalle de casos más abajo.
2. **`f93_concurrency_test`** (F101, sin cambios de fondo).
3. **`f87_logger_concurrency_test`** (nuevo, TSan, standalone — ver F87
   más abajo).
4. (implícito) el build de ESP32 real vía `idf.py build`, no afectado
   por nada de esto.

**Lección de rendimiento, aplicable a cualquier build de este
directorio en el futuro**: `CMakeLists.txt` nunca fijaba
`CMAKE_BUILD_TYPE`, que por defecto en CMake es **vacío** (sin
optimización, equivalente a `-O0`) si no se especifica. El test de DH de
`unit_tests` (ver más abajo) hace 80 llamadas reales a `modExp()`
(`BigUint`, F100) — a `-O0` eso solo ya tardaba **~40 segundos**, dando
la falsa impresión de que "los unit tests son lentos". Agregado
`if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Release) endif()` al
principio del archivo — bajó a **~4.3 segundos** limpio. Verificado
ambos números directamente, no asumido.

**Casos de `unit_tests`** (todos contra el código real, no
reimplementaciones):

- **`Utils.cpp`**: `stringHexToBytes`/`bytesToHexString` (round-trip),
  `bigNumAdd`/`bigNumMultiply`/`bigNumDivide` (aritmética base-256
  big-endian con acarreo, vectores calculados a mano y verificados),
  `h2int`, `urlDecode`, `hton64` (round-trip), `pack<T>`/`extract<T>`.
- **`BigUint`/DH (F100)**: como `BigUint` vive en un namespace anónimo
  dentro de `Crypto.cpp` (sin linkage externo), se testea a través de la
  API pública real de `CryptoMbedTLS` — (1) un **vector de respuesta
  conocida**: `privateKey` fijada a mano (miembro público, se puede
  sobreescribir sin tocar `dhInit()`), `dhCalculateShared()` contra un
  `remoteKey` fijo, comparado byte a byte contra `remoto^privado mod p`
  calculado independientemente con `pow()` de Python — coincide exacto;
  (2) **20 intercambios DH completos** entre dos `CryptoMbedTLS`
  independientes (`dhInit()` real, con clave privada aleatoria real cada
  vez), confirmando que ambas partes llegan siempre al mismo secreto.
  Esto formaliza como test real la verificación ad-hoc que ya se había
  hecho al escribir F100.
- **Bounds-checks**: **F21** (`AuthChallenges::prepareAuthPacket`
  rechaza `authData` de 513 bytes, acepta exactamente 512 — el límite
  real de `LoginCredentials.auth_data.bytes`) y **F35**
  (`LoginBlob::loadZeroconf` rechaza un blob ZeroConf con checksum HMAC
  incorrecto — `decodeBlob()` es privado, se llega por la API pública).
  **F22 quedó explícitamente diferido**: `readBlobInt()` solo es
  alcanzable construyendo un blob doblemente cifrado válido completo, y
  eso requeriría **cifrar** con AES-ECB — `CryptoMbedTLS` solo expone
  `aesECBdecrypt()` públicamente, no hay una `aesECBencrypt()` simétrica
  para construir el caso de test. Se podría agregar (cambio chico,
  simétrico a lo que ya existe) si en algún momento se quiere cerrar
  este hueco.
- **Protobuf**: round-trip encode/decode de `ClientHello`,
  `APResponseMessage`, `APWelcome` (`NanoPBHelper`).
- **F86/CDNAudioFile** (ver detalle abajo).
- **F34** (`PlainConnection::recvPacket()`): un listener TCP crudo de
  "un solo disparo" (sin handshake, sin `FakeAP` — este chequeo corre
  *antes* de cualquier capa de cifrado/autenticación) manda un prefijo
  de tamaño de 2 (por debajo del mínimo de 4, dispararía un underflow
  sin la guarda) y de `0xFFFFFFFF` (implausiblemente grande) — ambos
  deben ser rechazados con excepción; un tamaño válido de 4 (paquete
  vacío, el caso límite más chico legal) debe aceptarse, confirmando que
  la guarda no rechaza también tráfico válido.

**F87 — nuevo test Tier 2, binario propio**
(`f87_logger_concurrency_test.cpp`): 12 hilos × 100 llamadas a
`bell::bellGlobalLogger->info(...)` concurrentes, con un mensaje que
codifica hilo+secuencia. Se captura stdout por pipe (con
`F_SETPIPE_SZ` para agrandarlo a 1MB — al volumen de líneas de este
test, el buffer de pipe por defecto de Linux, 64KB, se llenaría y
bloquearía un hilo escritor a mitad de una línea de log, indistinguible
del bug que se busca) y se verifica con una regex ancorada que **cada
línea completa** es exactamente una llamada sin corromper — no solo
"no crasheó". Vive en su **propio binario bajo TSan**, separado de
`unit_tests`: la primera versión lo tenía adentro, pero eso forzaba
*todo* `unit_tests` (incluido el test de DH con sus 80 `modExp()`
reales) a pasar por la instrumentación de TSan, convirtiendo la suite
"rápida" de ~4s en ~44s sin ningún beneficio para los tests que no son
de concurrencia.

**F85/F86 — `CDNAudioFile` contra un servidor CDN falso** (adentro de
`unit_tests`, no necesita TSan — no hay carrera real que buscar acá):

- **Bloqueador real, ya resuelto (2026-07-17)**: tanto `CDNAudioFile.cpp`
  como `TrackQueue.cpp` dependen de `HTTPClient`/`SocketStream`
  (`bell/main/io/`), y `SocketStream.cpp` construye un `bell::TLSSocket`
  real en la rama HTTPS — el **linker** exigía el símbolo aunque los
  tests solo usaran `http://`. El bloqueador de portabilidad era un solo
  include: `esp_heap_caps.h` sin guarda de `ESP_PLATFORM`, detrás de
  `logInternalHeap()` (ya documentado en F101). Con `logInternalHeap()`
  eliminado (F64 dejó de necesitarlo), `TLSSocket.cpp` no tiene código
  ESP-específico y compila para host sin modificaciones — se usa el
  `.cpp` real (no un stub), igual que `main/platform/linux/X509Bundle.cpp`
  (el backend de verificación de certificados por plataforma que ya
  existía; en Linux `shouldVerify()` devuelve `false`, sin verificación
  de cadena — mismo comportamiento que tenía el stub viejo). El antiguo
  `TLSSocketStub.cpp` (mismo patrón que `ApResolveStub.cpp`: satisfacer
  al linker, tirar excepción si algo lo usa de verdad) se borró — ya no
  hace falta, y esto suma cobertura HTTPS real a la suite (antes
  imposible de ejercitar en host).
- **`fake_cdn_server.h`/`.cpp`** (nuevo): servidor HTTP/1.1 real y
  mínimo — entiende `GET` con header `Range` (incluye el formato
  "sufijo", `bytes=-N`, que usa `RangeHeader::last()` para el footer),
  y soporta **keep-alive** (`CDNAudioFile` reusa una única conexión para
  header+footer+cuerpo, ver F58/F82 — hace falta de verdad para que
  `openStream()` funcione). Un enum `Mode` reproduce a propósito los dos
  comportamientos legales-pero-no-conformes que F86 defiende:
  `IgnoreRangeReturn200` (ignora `Range`, responde `200` con el archivo
  completo) y `LieContentLength206` (responde `206` real con el slice
  real, pero declara un `Content-Length` mucho mayor al que en verdad
  manda).
- **Fixture cifrado real**: un archivo falso de 50167 bytes (167 de
  header Opus + 50000 de payload), cifrado con AES-CTR real
  (`CryptoMbedTLS::aesCTRXcrypt`, la misma IV real de audio) en **una
  sola pasada completa** desde `pos=0` — como CTR es cifrado por bloque
  independiente y `CDNAudioFile::decrypt()` deriva su propia IV por
  posición (`bigNumAdd(audioAESIV, pos/16)`) en cada llamada
  (header/footer/cuerpo por separado), cifrar todo de una vez alcanza:
  cualquier slice que `decrypt()` reciba después, sin importar cómo se
  trocee, descifra al valor correcto.
- **Tres casos**: (1) camino sano completo — `openStream()` +
  `readBytes()` contra un servidor que respeta `Range`, bytes
  descifrados comparados byte a byte contra el plaintext original; (2)
  `openStream()` debe **tirar** si el servidor devuelve `200` en vez de
  `206` (chequeo de status, primera capa de F86); (3) `readBytes()`
  tras un `seek()` (fuera del prefetch de header/footer, fuerza un
  pedido de rango nuevo) contra un servidor que miente el
  `Content-Length` — debe devolver exactamente `httpBuffer.size()`
  bytes bien descifrados, ni más ni menos (el clamp, segunda capa de
  F86).
- **"Probar el test" — verificación real, no solo escrita y asumida**:
  se desactivó el clamp (`lastRequestCapacity = contentLength()` en vez
  del `std::min(...)`) temporalmente y se confirmó que el caso (3)
  efectivamente lo detecta. Primer intento: el helper `withTimeout()`
  (pensado para no colgar toda la suite si el cliente se queda esperando
  bytes que el servidor mentiroso nunca manda — `TCPSocket::read()` no
  tiene timeout propio) usaba `std::async` + `wait_for()`, pero el
  **destructor de `std::future` bloquea hasta que la tarea termina**
  incluso después de que `wait_for()` avisó que se pasó de tiempo — el
  timeout no cortaba nada, solo pateaba el mismo cuelgue unos
  microsegundos después. Corregido con un `std::thread` crudo +
  `detach()` en la rama de timeout (documentado en el propio código: el
  hilo abandonado puede tocar referencias colgantes si el proceso sigue
  vivo, pero eso solo pasa si el fix bajo test ya está roto, en una
  corrida que un `timeout` externo debería matar de todas formas).
  Con el fix real corregido, se re-verificó: la detección del clamp
  ahora sí reporta el fallo en 5s en vez de colgar. Revertido el clamp a
  su estado real antes de seguir.

**F76/F78 — evaluados, diferidos explícitamente** (no implementados,
documentado para no repetir el análisis en el futuro):

- **F76** (`cspot_connect.cpp`, `lastTrackId`/`pcmWrite()`): vive en la
  capa de integración de la app (`components/cspot/`, no el motor
  vendorizado), fuertemente acoplada al sink de audio I2S real de ESP32
  — no es razonable portar a host sin mockear una porción grande de la
  capa de audio.
- **F78** (`TrackQueue::preloadedTracks`): el camino que dispara el bug
  (`processTrack()`'s caso `CDN_REQUIRED` → `stepLoadCDNUrl()`) pasa por
  `AccessKeyFetcher`, que depende a su vez de `MercurySession` (para el
  token) *y* `HTTPClient` (para el storage-resolve *y* el propio CDN) —
  un test Tier 2 real para esto necesitaría combinar la infraestructura
  de `FakeAP` (F101) con un endpoint de token falso *y* uno de
  storage-resolve falso además del CDN falso de F86, sustancialmente más
  grande que cualquiera de los dos bloques ya construidos esta sesión.
  Candidato claro para una sesión futura dedicada, reusando
  `fake_cdn_server.h` como base.

**Verificado**: los cuatro binarios compilan y corren limpios
(`unit_tests`: 24/24, ~4.3s; `f87_logger_concurrency_test`: TSan limpio,
~0.04s; `f93_concurrency_test`: sin cambios de fondo, sigue limpio bajo
TSan). `idf.py build` para ESP32 limpio, 0 errores, mismo margen de
flash (~52% libre) — nada de este directorio se toca ni se linkea con
el firmware real.

**Cómo compilar y correr todo**:

```
cd components/cspot/tests
cmake -S . -B build
cmake --build build -j
./build/unit_tests                                          # ~4s
TSAN_OPTIONS="halt_on_error=0" ./build/f87_logger_concurrency_test  # ~instantáneo
TSAN_OPTIONS="die_after_fork=0" ./build/f93_concurrency_test 30     # ~30s
```

**Addendum (mismo día) — interrupción de conexión CDN (F58) e
investigación de "bytes de más" (F82)**: a pedido del usuario, dos casos
más agregados a `unit_tests`/`fake_cdn_server.{h,cpp}`.

- **F58, `killCurrentConnection()`**: mismo patrón que `FakeAP` (F101) —
  ahora `FakeCdnServer` puede cortar la conexión activa a mitad de
  sesión. El test hace `openStream()` + un `readBytes()` normal
  (`connectionCount()==1`), corta la conexión, y fuerza otro
  `readBytes()` a una posición nueva — confirma que
  `CDNAudioFile`/`HTTPClient` reconectan solos y devuelven los bytes
  correctos, **y** que de verdad hubo una conexión TCP nueva
  (`connectionCount()==2` — sin este chequeo el test no distinguiría un
  corte real de un corte que no tuvo ningún efecto).

- **F82, investigación real, no asumida**: pregunta del usuario — ¿qué
  pasa si el servidor manda **más** bytes de los que declaró en
  `Content-Length` de una respuesta `206` (lo opuesto al caso que F86 ya
  cubre)? Se agregó `FakeCdnServer::Mode::ExtraBytesAfter206` (declara
  un `Content-Length` verdadero, pero escribe 256 bytes de basura justo
  después, antes de que la conexión quede lista para la próxima
  request) y se corrió el escenario para ver qué pasaba de verdad, no
  para confirmar una hipótesis ya decidida.
  - **Primer intento del test: quedó colgado.** No era un bug de
    `CDNAudioFile` — era que `FakeCdnServer` solo atendía **una
    conexión a la vez** (`accept()` bloqueaba hasta que la conexión
    actual se cerraba). El cliente real, al toparse con los bytes de
    basura, tira una excepción de parseo, reintenta abriendo una
    **conexión nueva** — pero recién destruye el objeto `Response`
    viejo (y por lo tanto cierra esa conexión) **después** de que la
    nueva ya esté completamente armada (`bell::HTTPClient::get()`
    corre entero antes de la asignación `this->httpConnection = ...`).
    Con las dos conexiones necesitando estar abiertas a la vez aunque
    sea un instante, un server de un solo hilo se traba. Corregido:
    `FakeCdnServer` ahora atiende **una conexión por hilo**
    (`std::thread` + `detach()` por cada `accept()`), como un servidor
    real. Confirmado con `connectionCount()` que las dos conexiones
    efectivamente coexistieron.
  - **Con eso corregido, el resultado real**: los bytes de basura sí
    desincronizan el parseo de la *próxima* respuesta en esa misma
    conexión keep-alive (log real:
    `"Cannot parse http response"`) — pero el mismo mecanismo de
    reintento que ya existe en `HTTPClient::Response::rawRequest()`
    (F82, el chequeo `!socketStream.isOpen() || !socketStream.good()`)
    ya lo atrapa y reconecta solo, sin ningún cambio de código de
    producción necesario. Es el mismo mecanismo de resiliencia que
    F58 ejercita por un disparador distinto (conexión cortada vs.
    conexión desincronizada) — no un bug nuevo, una propiedad de F82
    que no estaba verificada explícitamente para este caso puntual y
    ahora sí lo está (`CHECK(server.connectionCount() > 1)` confirma
    que el reintento automático realmente disparó, no que la basura no
    tuvo ningún efecto).

**Verificado**: `unit_tests` sigue en 26/26 (107 asserts), corrido 4
veces seguidas sin flakiness (~9-10s con estos dos casos nuevos,
incluyen reintentos de red reales). `idf.py build` para ESP32 limpio
después de este addendum también (nada de esto toca código de
producción, solo `tests/`).

---

### F103 — Quinto binario `engine_test`: TrackQueue/TrackPlayer/DealerSession/PlayerEngine compilando y linkeando para host (Hardening/testing, aplicado)

Hasta acá `TrackQueue.cpp`/`TrackPlayer.cpp`/`DealerSession.cpp`/
`PlayerEngine.cpp`(+`PlayerEngineCommands.cpp`, ver
`docs/dealer_websocket_migration.md` §7) quedaban totalmente fuera de
`tests/` — no por ninguna dependencia real de ESP32 (ninguno de los
cinco archivos tiene un solo `#ifdef ESP_PLATFORM` ni llama una API
`esp_*`), sino porque nadie los había cableado en el CMake de host
todavía. El usuario preguntó explícitamente "no deberíamos depender de
ESP32" y pidió hacerlo ("es importante darle cobertura en testing").

**Lo que hacía falta agregar** (todo portable, nada específico de
ESP32):
- `TrackReference.cpp`, `AccessKeyFetcher.cpp`, `ApResolve.cpp`,
  `Login5Client.cpp`, `ContextResolver.cpp` — la cadena de red que
  `PlayerEngine` necesita antes de llegar a los cuatro
  archivos objetivo.
- `tremor` (decodificador Vorbis entero, C puro) y `libhelix-mp3`
  (decodificador MP3, C puro) — las mismas fuentes que
  `bell/CMakeLists.txt` ya compila para el build real de ESP32
  (`file(GLOB ...)` menos `ivorbisfile_example.c`), simplemente nunca
  se habían compilado para host. `TrackPlayer.cpp` las llama
  directo, no a través de la abstracción `bell::AudioCodecs.cpp` (esa
  es para otro camino de reproducción), así que no hizo falta ese
  archivo ni sus defines `BELL_CODEC_*`.
- `MP3Decoder.cpp`/`BaseCodec.cpp` (bell, con su cadena de headers
  `AudioContainer.h`→`StreamInfo.h`) y `NanoPBExtensions.cpp`
  (`AccessKeyFetcher.cpp` los necesita).
- `WrappedSemaphore.cpp` — faltaba el backend Linux
  (`bell/main/platform/linux/`); el header ya estaba
  ESP/Apple/Win32/Linux-guardado, solo no se había linkeado nunca la
  implementación para host.
- `connectstate.pb.c`/`clienttoken.pb.c` — ya generados por el build
  real de ESP32, solo sumados a la lista de fuentes.

**Por qué es un target aparte, no parte de `unit_tests`**: estos
archivos necesitan el `ApResolve.cpp` real (`fetchFirstSpclientAddress()`/
`fetchFirstDealerAddress()`, que `DealerSession`/`PlayerEngine`
llaman) — pero `unit_tests`/`f93_concurrency_test`/
`f87_logger_concurrency_test` linkean `ApResolveStub.cpp` (el doble de
F101, solo cubre `fetchFirstApAddress()`). Compilar ambos en el mismo
binario choca por ODR (dos definiciones de `ApResolve::ApResolve()`).
`engine_test` es su propio ejecutable exactamente por eso.

**Alcance real, sin exagerar**: esto es cobertura de **compilación +
link**, no de comportamiento — confirma que las 40+ fuentes nuevas
encajan (tipos, símbolos, includes) sin necesitar el toolchain de
ESP32, algo que antes solo se sabía después de un `idf.py build`
completo. Además se agregaron unos pocos `TEST_CASE` reales de
`doctest` para la parte de estas clases que es lógica pura, sin red:
`TrackReference::encodeURI()`/`decodeURI()` (ida y vuelta base62↔gid)
y `ContextResolver::trackFromJson()` (los tres casos: gid en base64,
solo uri con fallback base62, ninguno de los dos). Testear el
comportamiento en runtime de `DealerSession`/`PlayerEngine`
(reconexión, PUTs, comandos) seguiría necesitando algo del calibre de
`FakeAP`/`FakeCdnServer` de F101 pero para un servidor Dealer WS falso
— no se hizo acá, queda como posible extensión futura si hace falta.

**Verificado**: `engine_test` compila y linkea limpio (~40 fuentes
nuevas), 5/5 tests doctest pasando (15 asserts). Los otros cuatro
binarios (`unit_tests` 33/33, `f93_concurrency_test`,
`f87_logger_concurrency_test`, `ws_transport_echo_test`) siguen
pasando sin cambios — `CSPOT_SHARED_SOURCES`/`CSPOT_CDN_SOURCES` no se
tocaron, solo se agregó un set `CSPOT_ENGINE_SOURCES` nuevo y el
target propio.

