# Análisis del componente Spotify (CSpot / bell)

## 0. Alcance de este documento

Registra lo que fui encontrando al leer el código fuente mientras definía
este componente, copiado originalmente de `components/spotify` de
[`sle118/squeezelite-esp32`](https://github.com/sle118/squeezelite-esp32)
(rama `master-v4.3`, commit `0203682`, 2026-02-16), y las decisiones/cambios
que tomé para que compilara y funcionara como componente independiente en
este proyecto (ESP-IDF v6.0.1, target `esp32s3`).

A diferencia de una primera pasada "de lectura", **esto quedó verificado
compilando y linkeando de verdad** contra el toolchain de este proyecto
(`idf.py set-target esp32s3` + `ninja`), iterando sobre los errores reales
del compilador hasta llegar a un `idf.py build` **100% verde** — incluye un
port real de la capa criptográfica de `cspot`/`bell` a mbedTLS 4.0 (sección
2). Después de eso se probó en hardware real (una JC3248W535) y **la
sesión de Spotify quedó funcionando de punta a punta, con audio real
saliendo del parlante** — emparejamiento ZeroConf, autenticación con los
servidores de Spotify, y audio decodificado sonando (sección 5). En el
camino aparecieron varios bugs que ni compilar ni leer el código a mano
hubieran mostrado (F13-F20, todos con hardware real de por medio). La
sección 4 detalla el proceso de verificación por compilación, y la
sección 5 el resultado final en hardware — incluyendo F19/F20, los
hallazgos más recientes que **todavía no están confirmados en hardware**
(F18, el silencio total, resultó ser un conector suelto y no un bug de
software; F20, un glitch periódico de audio, tiene una corrección
aplicada pendiente de probar).

La sección 6 es distinta en espíritu a las anteriores: una auditoría de
código deliberada (no motivada por un síntoma en hardware) sobre el
propio código que mantenemos y sobre los archivos "core" de
sesión/autenticación de `cspot` que están en el camino crítico de este
producto — incluye, entre otras cosas, una cadena de desborde de buffer
(F21+F22) alcanzable sin autenticación desde la red local, vía el propio
endpoint de emparejamiento ZeroConf que expone este componente. De los 30
hallazgos de esa sección, 21 ya están corregidos y verificados
compilando (ver la tabla resumen al principio de la sección 6 para el
estado de cada uno); los 9 restantes quedan documentados sin aplicar.

La sección 7 (F51) es distinta otra vez: no es un hallazgo encontrado
sino un refactor pedido explícitamente — portar los sinks de audio de
`bell/main/audio-sinks/esp` (los que F1 documentó usando el driver I2S
legacy) a `driver/i2s_std.h` directamente dentro de `bell/`, para poder
retirar el `i2s_audio_sink.cpp` propio que hacía lo mismo por fuera.
**Confirmado en hardware.** F52 y F53, encontrados en esa misma prueba de
hardware (pausar desde el cliente de Spotify no detenía el audio, y
luego, al corregir eso, un efecto de "disco rayado" en vez de silencio
durante la pausa) están corregidos — F52 confirmado en hardware, F53
pendiente de confirmar.

Todas las rutas de archivo son relativas a `components/cspot/`.

**Nota sobre la estructura del componente**: la primera versión de este
componente (la que describen la mayoría de los hallazgos de abajo) copiaba
`components/spotify` de squeezelite-esp32 a una carpeta `vendor/` intacta,
y aplicaba todos los fixes de mbedTLS 4.0/hardware como archivos "sombra"
en `mbedtls4_compat/`/`upstream_fixes/` que el `CMakeLists.txt` inyectaba
por encima de las fuentes vendorizadas (vía trucos de CMake:
`get_target_property`/`list(FILTER)`/`target_include_directories(...
BEFORE ...)`). Ese patrón se abandonó después: `vendor/`/`mbedtls4_compat/`/
`upstream_fixes/` ya no existen — el motor `cspot`/`bell` vive directamente
en `cspot/` dentro de este componente, y todos los fixes descritos abajo
están aplicados en el propio código (`external/bell/main/utilities/Crypto.cpp`,
`external/bell/main/io/TLSSocket.cpp`, `external/bell/main/io/X509Bundle.cpp`,
`src/PlainConnection.cpp`), no en archivos paralelos. Las rutas de
este documento reflejan esa ubicación actual; donde un hallazgo menciona
`mbedtls4_compat/` o `upstream_fixes/` como mecanismo, es historia de cómo
se aplicó originalmente el fix, no la estructura vigente.

## 1. Qué se copió y qué se construyó

| Ruta | Origen | Se compila | Se modificó |
|---|---|---|---|
| `cspot/`, `external/bell/` | Copiado de `components/spotify/vendor/cspot` de squeezelite-esp32 (motor `cspot`/`bell`, vendorizado directamente ahí, no son submódulos git) | Sí, vía `add_subdirectory()` desde `CMakeLists.txt` de este componente | Sí — los fixes de mbedTLS 4.0 (sección 2) y de hardware (F17) están aplicados directamente en `external/bell/main/utilities/Crypto.{h,cpp}`, `external/bell/main/io/{TLSSocket,X509Bundle}.*` y `src/PlainConnection.cpp`; también un ajuste de `external/bell/CMakeLists.txt` (ver F1b) |
| `CMakeLists.txt`, `idf_component.yml`, `Kconfig` | Nuevo | Sí | — |
| `cspot_connect.{h,cpp}` | Nuevo, adaptado del `Shim.cpp` de squeezelite-esp32 (no incluido en este repo — ver más abajo) | **Sí, compila y linkea limpio** | — |
| `i2s_audio_sink.{h,cpp}` | Nuevo, sin equivalente reutilizable en `cspot/bell` (ver F1) | **Sí, compila limpio, verificado** | — |

El resto del `components/spotify` original de squeezelite-esp32 (`Shim.cpp`,
`cspot_sink.{c,h}`, `cspot_private.h`, `client_info.h`, `linker.lf`, su
propio `CMakeLists.txt`) **no se copió** — incluían cabeceras propias de
squeezelite-esp32 (`platform_config.h`, `accessors.h`,
`network_services.h`, `http_server_handlers.h`, `display.h`, `tools.h`,
`nvs_utilities.h`) y enlazaban contra componentes suyos que no existen acá,
así que no compilaban de todos modos. La parte reutilizable era `cspot/`
(protocolo Spotify Connect) y `external/bell/` (su capa de plataforma).
`cspot_connect.cpp` reimplementa la lógica de aquel `Shim.cpp` (clase
`cspotPlayer`) contra esa base, sin dependencias de squeezelite-esp32, y
las credenciales de `client_info.h` pasaron a ser opciones de Kconfig
(`CONFIG_CSPOT_CLIENT_ID`/`CONFIG_CSPOT_CLIENT_SECRET` — ver F8).

## 2. Hallazgo principal: mbedTLS 4.0 rompía la capa criptográfica de `cspot`/`bell` — portado (Crítico → resuelto, confirmado compilando y linkeando)

Este fue, con diferencia, el hallazgo más importante — descubierto recién al
compilar de verdad, no al leer el código. **Ya está portado**, directamente
en `external/bell/main/utilities/Crypto.{h,cpp}` y
`external/bell/main/io/{TLSSocket,X509Bundle}.*`.

**ESP-IDF v6.0.1 (este proyecto) trae mbedTLS 4.0.0** (release del
2025-10-15, reescritura mayor sobre la nueva base "TF-PSA-Crypto"):

```
$ grep MBEDTLS_VERSION_STRING .../mbedtls/include/mbedtls/build_info.h
#define MBEDTLS_VERSION_STRING         "4.0.0"
```

`cspot`/`bell` fueron escritos contra la API "clásica" de mbedTLS 2.x/3.x
(contextos de RNG manuales, `mbedtls_ctr_drbg_*`, `mbedtls_entropy_*`,
`mbedtls_aes_context`, campos directos de `mbedtls_x509_crt`, HMAC vía
`mbedtls_md_hmac_*`). Compilando el proyecto tal cual, 15 unidades de
traducción fallaban por esto (14 + `cspot_connect.cpp` una vez resuelto lo
demás). Evalué la [guía de migración oficial](https://github.com/Mbed-TLS/mbedtls/blob/development/docs/4.0-migration-guide.md)
y la de [TF-PSA-Crypto](https://github.com/Mbed-TLS/TF-PSA-Crypto/blob/development/docs/1.0-migration-guide.md)
contra el código real, y resultó bastante más acotado de lo que parecía leyendo
solo los errores del compilador — el esfuerzo real estuvo muy concentrado.
Desglose de lo que se encontró y cómo se resolvió, aplicado directamente en
los archivos reales de `cspot/bell`:

| Símbolo roto | Dónde | Qué se hizo |
|---|---|---|
| `mbedtls/bignum.h` (`mbedtls_mpi_*`, usado por `dhInit`/`dhCalculateShared` para el DH de 768 bits propio de Spotify) | `Crypto.cpp` | **Nada que portar.** ESP-IDF re-expone `mbedtls_mpi_*` completo como header público propio (`components/mbedtls/port/include/mbedtls/bignum.h`, porque lo necesitan para su acelerador RSA por hardware). La función que más preocupaba (modexp con un primo no estándar, que PSA en general no cubre) compila tal cual sin cambios. |
| `mbedtls/aes.h` (`aesCTRXcrypt`, modo CTR) | `Crypto.cpp` | Portado a la API de cifrado de PSA (`psa_cipher_decrypt_setup`/`_set_iv`/`_update`/`_finish` con `PSA_ALG_CTR`). **No** se reusó la librería `tiny-AES-c` ya vendorizada para `aesECBdecrypt` (mismo archivo, código sin tocar) porque esa copia está compilada para una única longitud de clave fija (`#define AES192 1`, 24 bytes) mientras que los dos llamadores reales de `aesCTRXcrypt` (`CDNAudioFile.cpp`, `LoginBlob.cpp`) usan claves de 16 bytes — reusarla habría sido un bug de corrección silencioso. PSA soporta cualquier tamaño de clave AES con una sola API. |
| `mbedtls_md_hmac_{starts,update,finish}` (HMAC-SHA1) | `Crypto.cpp` | Estas funciones **siguen declaradas** en `md.h` pero detrás de `#if defined(MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)` — ya no son API de aplicación ("HMAC operations are no longer supported via MD", comentario del propio header). Portado a `psa_mac_compute()` con `PSA_ALG_HMAC(PSA_ALG_SHA_1)` (API de un solo paso, más simple que el original de 3 llamadas). SHA1 simple (`mbedtls_md_init/setup/starts/update/finish`) sí siguió andando sin cambios. |
| `mbedtls/pkcs5.h` (`pbkdf2HmacSha1`) | `Crypto.cpp` | Portado a `psa_key_derivation_*` con `PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_1)` (cost → salt → password, en ese orden, como exige la propia macro de PSA). |
| `mbedtls/ctr_drbg.h` + `mbedtls/entropy.h` (`generateVectorWithRandomData`) | `Crypto.cpp` | Portado a `psa_generate_random()` (portable, sin rama por plataforma). En ESP-IDF esto ya termina llamando al TRNG por hardware igual que `esp_fill_random()` directo — `esp_hardware.c` conecta `psa_generate_random()` a `esp_fill_random()` sin ninguna capa de DRBG en el medio (`MBEDTLS_PSA_DRIVER_GET_ENTROPY`, ver `docs/aprendizaje.md`, entrada "Seguridad: TLS, verificación de certificados y mbedTLS", sección 4.1) — así que no hacía falta un `#ifdef ESP_PLATFORM` para elegir entre ambas. |
| Contextos RNG manuales + `mbedtls_ssl_conf_rng()` | `TLSSocket.cpp` | `mbedtls_ssl_conf_rng()` fue **eliminada** de la librería (TLS ahora siempre usa el RNG interno de PSA) — no había nada que portar, solo borrar el contexto `ctr_drbg`/`entropy` (miembros de la clase, en el `.h`) y esa llamada. El resultado queda más corto que el original. Se agregó `psa_crypto_init()` una vez, vía `external/bell/main/utilities/include/psa_init.h` (un `static` de inicialización perezosa, thread-safe por semántica de C++11). |
| `mbedtls_pk_can_do()` → `mbedtls_pk_can_do_psa()` + campo `sig_opts` (ya no existe en `mbedtls_x509_crt`) | `X509Bundle.cpp` (`crtCheckCertificate`) | **No portado.** La firma cambió de verdad (`mbedtls_pk_can_do_psa(pk, psa_algorithm_t, psa_key_usage_t)` en vez de `(pk, mbedtls_pk_type_t)`) y hay que reconstruir el `psa_algorithm_t` correcto a partir de `sig_md`/`sig_pk` del certificado — es lógica de verificación de cadena de certificados, así que un error ahí es un error de seguridad, no solo de compilación; necesita a alguien cómodo con identificadores de algoritmo PSA, no una traducción mecánica. Como `X509Bundle::init()` nunca se llama en ningún punto de squeezelite-esp32 (verificado por grep — ver F2), esta lógica está inerte de todas formas hoy. El contenido de `X509Bundle.cpp` se reemplazó por un stub que da `shouldVerify() = false` y deja `attach()`/`init()` como no-ops — mismo comportamiento en la práctica que trae upstream (TLS sin verificar certificado del servidor), sin fingir que hay una implementación real. |

**Cómo quedó aplicado**: directamente en los tres archivos —
`external/bell/main/utilities/Crypto.{h,cpp}`,
`external/bell/main/io/TLSSocket.{h,cpp}` y `external/bell/main/io/X509Bundle.cpp`
— sin ningún mecanismo de CMake para inyectar/excluir fuentes; el
`CMakeLists.txt` de este componente no necesita saber nada de esto, es
transparente para `add_subdirectory(cspot)`.

**Si en algún momento se quiere terminar `X509Bundle.cpp`** (verificación
real de certificado TLS, hoy deshabilitada — ver F2) hay dos caminos: portar
`crtCheckCertificate()` a `mbedtls_pk_can_do_psa()` de verdad, o —
probablemente mejor y menos trabajo— reemplazar todo `X509Bundle.cpp` por
`esp_crt_bundle_attach()` de ESP-IDF, que ya viene mantenido al día con
mbedTLS y trae su propio bundle de CAs curado.

## 3. Otros hallazgos

### F1 — Los sinks de audio ESP32 de `bell` usan una API de I2S que ya no existe (Alto, mitigado — ver actualización)

`external/bell/main/audio-sinks/esp/*.cpp` incluyen `driver/i2s.h`, el
driver I2S "legacy", removido en ESP-IDF v5.4+/v6 (confirmado: ese header no
existe en este proyecto).

**Mitigación original** (superada, ver abajo): `BELL_DISABLE_SINKS ON` +
`i2s_audio_sink.{h,cpp}` propio sobre `driver/i2s_std.h`, fuera de `bell/`.

**Actualización (F51)**: en vez de mantener un sink propio al margen de
`bell`, `BufferedAudioSink`/`PCM5102AudioSink` (la base compartida y el
sink genérico de DAC plano) se portaron directamente a `driver/i2s_std.h`
dentro de `bell/`, y el archivo propio se retiró. Sigue sin cubrir las
variantes con códec I2C (ES8388, ES8311, AC101, TAS5711, ES9018) ni
SPDIF — esas siguen en `driver/i2s.h` y excluidas del build. Ver sección 7
para el detalle completo.

Nota de API (sigue vigente): `i2s_chan_config_t::id` en esta versión de
ESP-IDF es un `int` llano, no el antiguo enum `i2s_port_t`.

### F1b — Los toggles `BELL_CODEC_*` no excluyen realmente los `.cpp` de los códecs deshabilitados (Medio, mitigado)

`external/bell/CMakeLists.txt:167` hace
`file(GLOB EXTRA_SOURCES "main/audio-codec/*.cpp" ...)`, que agrupa
`AACDecoder.cpp`/`OPUSDecoder.cpp`/`MP3Decoder.cpp` **sin condición**. Los
flags `BELL_CODEC_AAC/MP3/OPUS` sólo controlan si se agrega la librería
externa correspondiente (`add_subdirectory(external/opus)`, etc.), no si el
propio `.cpp` se compila. Con esos flags en OFF (como los dejé, ver F en
`CMakeLists.txt`), `AACDecoder.cpp`/`OPUSDecoder.cpp` fallan al no encontrar
`pvmp4audiodecoder_api.h`/`opus.h`. Mismo problema con
`EncodedAudioStream.h:9`, que incluye `mp3dec.h` sin guardarlo tras
`BELL_CODEC_MP3`.

**Mitigación aplicada**: dos partes. `external/bell/CMakeLists.txt` (código
propio del motor, editado directamente — ver nota de estructura en la
sección 0) filtra `AACDecoder.cpp`/`OPUSDecoder.cpp` del glob de
`EXTRA_SOURCES` con `list(FILTER ... EXCLUDE REGEX ...)`, ya que igual se
agregan explícitamente unas líneas más abajo cuando el flag correspondiente
está `ON`. `EncodedAudioStream.h` sí quedó sin tocar (su miembro
`MP3FrameInfo mp3FrameInfo` no está condicionado a `BELL_CODEC_MP3`, así
que cambiar eso es un cambio de ABI de la clase, no solo de build) — en su
lugar, `CMakeLists.txt` de este componente agrega el include de
`libhelix-mp3` para satisfacer su `#include "mp3dec.h"` sin habilitar el
decoder MP3 en sí.

### F1c — El decoder Vorbis (`tremor`) no compila con `-Werror` en GCC 15 (Medio, mitigado)

`external/bell/external/tremor/*.c` (decoder Vorbis entero, código de
~20 años) dispara `-Wmisleading-indentation`, `-Wmaybe-uninitialized` y
`-Wshift-negative-value`, y el build de este proyecto trata warnings como
errores. Es exactamente el códec que necesitamos (Spotify solo manda Ogg
Vorbis). **Mitigación aplicada**: `-Wno-error` sólo para esos archivos, vía
`set_property(SOURCE ... DIRECTORY ... PROPERTY COMPILE_OPTIONS ...)` —
nota para quien toque esto: `set_source_files_properties()` sin `DIRECTORY`
no alcanza fuentes de un target definido en un `add_subdirectory()` más
profundo; hace falta la forma explícita con `DIRECTORY`, algo que me costó
diagnosticar (ver historial de commits de este archivo). **Compila limpio,
verificado.**

### F2 — Verificación de certificado TLS deshabilitada por defecto (Alto, seguridad — ✅ resuelto en ESP32, ver F55)

`external/bell/main/io/X509Bundle.cpp:28` arrancaba con
`s_should_verify_certs = false`, y sólo pasaba a `true` si algo llamaba a
`X509Bundle::init()` con un bundle real. Confirmado por grep: nadie lo llamaba
en `components/spotify`, `main/`, `components/services` ni
`components/platform_console` del `squeezelite-esp32` original. El stub que
reemplazó `X509Bundle.cpp` en este componente (sección 2) preservó
exactamente este comportamiento — `shouldVerify()` siempre devolvía
`false` — así que, una vez que la capa TLS compiló y linkeó, esto quedó
activo de verdad: toda conexión HTTPS de `bell` (login5, access-token, CDN)
aceptaba cualquier certificado sin validarlo. No era una regresión de este
port, era el comportamiento que trae upstream tal cual.

**Resuelto para ESP32 por F55** (sección 8): `X509Bundle` pasó a tener una
implementación por plataforma, y la de ESP32 delega en
`esp_crt_bundle_attach()` con verificación activada por defecto
(`CONFIG_CSPOT_TLS_VERIFY_CERTIFICATES=y`). Alcance de la corrección: solo
ESP32, que es la única plataforma que este proyecto compila y prueba —
Linux/Apple/Windows conservan el stub "no verifica nada" sin cambios, ver
F55 para el detalle.

### F3 — Herramientas de build necesarias y no instaladas por defecto (Alto, bloqueante — resuelto de forma permanente)

`CMakeLists.txt (repo root)` genera código a partir de los `.proto` de
`protobuf/` usando `nanopb_generate_cpp`, que necesita un `protoc` y el
paquete Python `protobuf`. En este entorno no había ninguno de los dos, y el
síntoma típico si falta el paquete Python es:

```
*************************************************************
*** Could not import the Google protobuf Python libraries ***
*** Try installing package 'python3-protobuf' or similar.  ***
*************************************************************
...
ModuleNotFoundError: No module named 'google'
```

Si en cambio el paquete está pero es incompatible con el `protoc` instalado
(protobuf valida en tiempo de ejecución que el "gencode" del compilador y el
runtime Python tengan la misma versión mayor), el error es distinto —
`google.protobuf.runtime_version.VersionError: Detected mismatched Protobuf
Gencode/Runtime major versions...` — y si `setuptools` es demasiado nuevo
(`pkg_resources`, removido en `setuptools>=81`, todavía lo importa el
generador de nanopb), es `ModuleNotFoundError: No module named
'pkg_resources'`. Los tres son el mismo problema de fondo: falta tooling de
protobuf compatible entre sí en el entorno Python que usa ESP-IDF.

**Complicación real encontrada**: esta instalación de ESP-IDF resuelve a
**dos entornos Python distintos** según cómo se invoque `idf.py`:

- `. $IDF_PATH/export.sh` en una shell manual activa
  `/home/user/.espressif/python_env/idf6.0_py3.13_env`.
- La extensión de ESP-IDF para VS Code (u otra forma de invocar `idf.py`
  que resuelva su propio `-DPYTHON=...`) usó en la práctica
  `/home/user/.espressif/tools/python/v6.0.1/venv` — confirmado viendo el
  `cmake ... -DPYTHON=.../tools/python/v6.0.1/venv/bin/python` en la salida
  real de un build que falló pese a que el primer venv ya tenía todo
  instalado.

Arreglar solo uno de los dos deja al otro roto según cómo se abra la
terminal — pasó exactamente eso la primera vez. **Solución permanente
aplicada, cubriendo ambos casos:**

1. Mismo `pip install "setuptools<81" "protobuf==5.27.2"
   "grpcio-tools==1.66.2"` corrido en **los dos venvs**:
   `python_env/idf6.0_py3.13_env` y `tools/python/v6.0.1/venv`. Esto
   resuelve el `import google.protobuf` de `nanopb_generator.py`
   sin importar cuál de los dos elija `idf.py` como `${PYTHON}` en una
   sesión dada.
2. Un `protoc` real todavía hace falta: `find_program(PROTOBUF_PROTOC_EXECUTABLE
   NAMES protoc ...)` en `FindNanopb.cmake` busca un ejecutable literal
   llamado `protoc` en el `PATH` del sistema — no alcanza con que
   `grpc_tools.protoc` esté instalado como módulo Python en algún venv, hay
   que poder invocarlo como comando. Para no depender de que el venv
   correcto esté al principio del `PATH` en cada sesión, el shim quedó en
   `~/.local/bin/protoc` (ya está en el `PATH` de este usuario
   incondicionalmente, se use o no `export.sh`):

   ```bash
   #!/usr/bin/env bash
   exec /home/user/.espressif/tools/python/v6.0.1/venv/bin/python -m grpc_tools.protoc "$@"
   ```

   (además se dejó una copia idéntica, apuntando a su propio `python`, en
   `bin/protoc` de cada uno de los dos venvs de arriba, por si algún día se
   quita `~/.local/bin` del `PATH`.)

Verificado desde cero **dos veces**, la segunda replicando el problema real
reportado (sesión sin las variables que yo había activado a mano
previamente): `idf.py fullclean && idf.py build` con nada más que
`. export.sh`, termina en 0.

**Alternativa** si se prefiere no tocar el entorno Python compartido de
ESP-IDF ni `~/.local/bin`: instalar `protoc` vía el gestor de paquetes del
sistema (p. ej. `apt install protobuf-compiler` en Debian/Ubuntu, necesita
privilegios de root que este entorno no tiene) y `pip install protobuf` con
una versión compatible con esa versión de `protoc` — confirmar con
`protoc --version` vs. el mensaje `VersionError` si no coinciden.

### F4 — `bell::Task` exige PSRAM (Medio, mitigado)

`external/bell/main/utilities/include/BellTask.h` reserva el stack de
cada tarea con `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
por defecto. Sin PSRAM habilitada, la asignación devuelve `NULL` y la
creación de tarea falla en silencio (sin log de error en `BellTask.h`).
**Mitigación aplicada**: `sdkconfig.defaults` habilita `CONFIG_SPIRAM=y`
(modo octal, pensado para ESP32-S3-DevKitC-1 N8R8/N16R8 — ajustar según la
placa real).

### F5 — Excepciones de C++ deshabilitadas por defecto rompen `bell` (Medio, mitigado, verificado)

`external/bell/main/io/*.cpp` (`URLParser.h`, `BinaryStream.cpp`,
`FileStream.cpp`, `X509Bundle.cpp`) usan `throw std::invalid_argument`/
`std::runtime_error`. Los proyectos ESP-IDF traen excepciones de C++
deshabilitadas por defecto (`-fno-exceptions`). **Mitigación aplicada**:
`CONFIG_COMPILER_CXX_EXCEPTIONS=y` en `sdkconfig.defaults`.

### F6 — El control de volumen por software está declarado pero no implementado en ningún sink vendorizado (Bajo/Medio, mitigado — ver actualización)

`AudioSink.h` declara `softwareVolumeControl = true` y
`virtual void volumeChanged(uint16_t)` sin cuerpo por defecto. Ningún sink
ESP32 vendorizado lo sobreescribe para atenuar PCM — solo reportan el
volumen de vuelta a Spotify.

**Mitigación original** (superada, ver abajo): `i2s_audio_sink.cpp`
propio implementaba una atenuación real (escalado de muestras de 16
bits, `std::clamp` para no desbordar).

**Actualización (F51)**: esa misma lógica de atenuación ahora vive en
`BufferedAudioSink::volumeChanged()`/`feedPCMFrames()`, compartida por
todos los sinks que heredan de esa base (hoy: `PCM5102AudioSink`) — ya
no depende de un archivo fuera de `bell/`. Sigue sin cubrir los sinks
con códec I2C todavía no portados (ver F1/sección 7).

### F7 — Sincronización de "inicio/fin de pista" simplificada respecto a upstream (Bajo, simplificación documentada en el propio código)

El `Shim.cpp` original de squeezelite-esp32 (no copiado a este componente —
ver sección 1), líneas 419-460, retrasa `notifyAudioReachedPlayback()`/
`notifyAudioEnded()` hasta que la posición dentro del buffer de salida de
squeezelite-esp32 (varios segundos) alcanza el offset correcto. Este
componente escribe casi directo al buffer DMA de I2S (decenas de ms), así
que esos eventos se aproximan a "en cuanto se entrega/termina de entregar
audio al sink" — ver comentario en `cspot_connect.cpp` (`pcmWrite`,
`eventHandler` caso `DEPLETED`).

### F8 — Registro de aplicación de Spotify: riesgo de producto, no de código (Medio)

Este componente necesita un `CLIENT_ID`/`CLIENT_SECRET` real de
[developer.spotify.com](https://developer.spotify.com/dashboard), configurados
vía `idf.py menuconfig` → `CSpot (Spotify Connect) component` →
`CONFIG_CSPOT_CLIENT_ID`/`CONFIG_CSPOT_CLIENT_SECRET` (Kconfig, no un
archivo separado — quedan en `sdkconfig`, que ya está en `.gitignore`).
Desde 2024 Spotify exige revisión/aprobación para acceso extendido más allá
de la cuenta del propio desarrollador — conviene verificar la política
vigente antes de asumir que cualquier app registrada funciona para
terceros.

### F9 — Pines I2S hardcodeados en los sinks vendorizados (Bajo, ✅ corregido para PCM5102AudioSink)

`PCM5102AudioSink.cpp:21-23` fijaba `bck_io_num=27, ws_io_num=32,
data_out_num=25` sin forma de configurarlos.

**Corrección aplicada (F51)**: `PCM5102AudioSink` (y su base
`BufferedAudioSink`) ahora toman un `Config` con los pines configurables
— la misma forma que ya usaba `cspot_connect_config_t`/`Kconfig` para el
sink propio que se retiró. Los sinks con códec I2C todavía no portados
(ES8388, AC101, etc. — ver F1/sección 7) siguen con pines fijos.

### F10 — `cspot_connect_stop()` no puede liberar memoria de forma segura (Bajo, limitación del adaptador nuevo)

`bell::Task` no expone `join()` ni señal de "la tarea terminó". Documentado
en el propio código: `cspot_connect_stop()` filtra el objeto a propósito
en vez de destruirlo, para no arriesgar un use-after-free si la tarea
FreeRTOS todavía la está usando. No es seguro llamar a
`cspot_connect_start()` de nuevo después.

### F11 — Licenciamiento mixto y sin cabeceras SPDX (Informativo, revisar antes de distribuir)

- `cspot` upstream ([`feelfreelinux/cspot`](https://github.com/feelfreelinux/cspot),
  `LICENSE.md`) es **GPL-3.0-or-later** (verificado).
- Ningún archivo `.cpp`/`.h` en `src`/`include`
  lleva cabecera de copyright/licencia.
- `squeezelite-esp32` no tiene `LICENSE` en la raíz del repo.
- `cspot_private.h`/`cspot_sink.h`, el pegamento propio de squeezelite-esp32
  (no copiado a este componente — ver sección 1), sí declaraban MIT/CC0,
  pero eso no cubre el motor de `cspot` en sí.
- `external/bell/external/libhelix-mp3` trae la RealNetworks Public Source
  License (no OSI, con restricciones) — no se compila en este proyecto
  (`BELL_CODEC_MP3 OFF`), pero está presente en el árbol.

**Conclusión práctica**: tratar este componente como GPL-3.0 de punta a
punta hasta confirmar lo contrario con los autores.

## 4. Verificación realizada (compilando y linkeando de verdad)

Se instaló ESP-IDF v6.0.1 vía `export.sh` y se corrió, iterando sobre los
errores reales:

```
idf.py set-target esp32s3
idf.py build   # y "ninja -C build -k 0" para ver todos los errores de una,
               # en vez de parar en el primero
```

**Estado final: `idf.py build` termina en 0 (build 100% verde).**
`build/cspot_example.bin` se genera y entra en la partición de
aplicación (1 483 648 bytes de 1 536 000 disponibles, 3% libre — ver F12).
Se llegó ahí en varias pasadas, cada una resolviendo lo que el compilador
reportaba de verdad (no señalado por lectura de código):

1. **~1150 unidades, 14 fallaban** — todas por la causa raíz de la sección 2
   (mbedTLS 4.0). `i2s_audio_sink.cpp` y `main/main.cpp` ya compilaban
   limpio en esta pasada.
2. Se implementó el port de la sección 2. Bajó a **1 falla**:
   `cspot_connect.cpp` no compilaba por un bug propio, no relacionado con
   mbedTLS — faltaba `#include "WrappedSemaphore.h"` (el tipo sólo estaba
   forward-declarado vía `CDNAudioFile.h`/`SpircHandler.h`, insuficiente
   para declarar el miembro `bell::WrappedSemaphore clientConnected` por
   valor). Corregido.
3. Con eso resuelto, compiló y **linkeó** todo — `cspot`, `bell`, este
   componente y `main/` — pero el chequeo final de tamaño de partición
   falló: el binario final (~1.48 MB) no entra en la partición "factory" de
   1 MB que trae por defecto cualquier proyecto ESP-IDF nuevo. Solución:
   `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` en `sdkconfig.defaults` (ver
   F12).
4. `idf.py build` limpio, 0 errores.
5. Verificado además **desde cero**, en una terminal nueva: `idf.py
   fullclean && idf.py build` con nada más que `. $IDF_PATH/export.sh`
   activado de antemano — sin exportar `PATH` a mano ni activar ningún venv
   aparte. Sigue en 0 errores (ver F3 para el detalle de qué se instaló de
   forma permanente para que esto sea así).

**Cambios hechos en el entorno Python de ESP-IDF de esta máquina** (no en
este repo, no reversibles con nada de este proyecto) para poder llegar
hasta acá — ver F3 para el detalle completo y por qué quedaron instalados
ahí en vez de en un venv aparte:
`/home/user/.espressif/python_env/idf6.0_py3.13_env`: `pip install
"setuptools<81" "protobuf==5.27.2" "grpcio-tools==1.66.2"`, más un shim
ejecutable `bin/protoc` en ese mismo venv.

**Actualización — sí se probó en hardware real** (una JC3248W535, ver F13):
bootea, conecta Wi-Fi, obtiene IP y llega a `cspot_connect_start()` sin
crashear — es decir, PSRAM, particiones, y el arranque general están bien.
Lo que quedó sin probar en la primera vuelta fue el descubrimiento
ZeroConf (F13, ya corregido) y, más allá de eso, todavía falta confirmar en
la práctica el emparejamiento completo, la reproducción de audio, y sobre
todo que el port de mbedTLS 4.0 (sección 2) efectivamente complete un
handshake TLS real contra los servidores de Spotify (`psa_cipher_*`/
`psa_mac_compute`/`psa_key_derivation_*` están implementados según la
documentación de la API, pero recién se puede ejercitar ese camino una vez
que el dispositivo aparezca en la app y alguien lo empareje).

### F12 — La partición "factory" por defecto (1 MB) no alcanza (Bajo, mitigado, verificado — ver F56 para la ampliación posterior)

Cualquier proyecto ESP-IDF nuevo arranca con la tabla de particiones
"Single factory app, no OTA" (1 MB para la app). El binario final de este
proyecto (cspot + bell + tremor + mbedTLS + Wi-Fi) pesa ~1.48 MB.
**Mitigación aplicada originalmente**: `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`
en `sdkconfig.defaults` (partición de 1500K). Esto alcanzó para entrar,
pero una vez agregada la verificación de certificados TLS (F55) el margen
bajó a solo 1% — **F56** (sección 8) reemplaza esta tabla de particiones
por una a medida con más espacio para la app, usando flash que ya estaba
disponible en el chip sin usar.

### F13 — El dispositivo nunca aparecía en la app de Spotify: `mdns_init()` nunca se llamaba (Alto, encontrado en hardware real, corregido)

Primer hallazgo encontrado con hardware de verdad (JC3248W535), no
compilando ni leyendo código en abstracto: el dispositivo bootea, conecta
Wi-Fi, y llega a imprimir "Spotify Connect device ... advertised, waiting
to be selected" — pero nunca aparece en el selector de dispositivos de la
app de Spotify.

**Causa**: `bell::MDNSService::registerService()`
(`external/bell/main/platform/esp/MDNSService.cpp`) llama directamente
a `mdns_service_add(...)` — pero **nunca llama a `mdns_init()`** en ningún
lado de todo `cspot`/`cspot/bell` (verificado por grep en
todo el árbol). El componente `mdns` de ESP-IDF exige `mdns_init()` antes
de poder agregar servicios; sin eso, el responder mDNS nunca arranca y
`mdns_service_add()` no tiene ningún efecto — sin devolver ningún error que
se pudiera haber notado, porque `cspot_connect.cpp` tampoco revisaba el
resultado. El log "advertised..." es solo un `ESP_LOGI` mío, no una
confirmación real de que el anuncio salió a la red; por eso todo parecía
funcionar en los logs y sin embargo el teléfono nunca veía nada.

Este era un bug en mi propia adaptación (`cspot_connect.cpp`), no algo
heredado de `cspot`/`bell` sin más — asumí que `bell::MDNSService` se
auto-inicializaba, y no es el caso: el diseño de esa clase da por sentado
que la aplicación que la usa ya trae su propio mDNS andando (razonable en
squeezelite-esp32, que sí lo hace en otro lado; no razonable asumirlo
tácitamente en un componente standalone).

**Corrección aplicada** (`cspot_connect.cpp`,
`startHttpServerAndMdns()`): agregar `mdns_init()` +
`mdns_hostname_set(deviceName)` + `mdns_instance_name_set(deviceName)`
antes de `bell::MDNSService::registerService(...)`. Compila limpio,
verificado — falta que el usuario confirme en la placa que el dispositivo
ya aparece en la app.

**Confirmado en hardware real**: con este fix el dispositivo sí aparece y
se puede seleccionar en la app — ver F14 para lo que pasó justo después.

### F14 — Crash (`LoadProhibited`) al emparejar: al body del POST de ZeroConf le faltaba decodificar `application/x-www-form-urlencoded` (Crítico, encontrado en hardware real, corregido)

Segundo hallazgo encontrado con hardware real: con F13 corregido, el
dispositivo aparece en la app y se puede seleccionar — pero al hacerlo, la
placa crashea con `Guru Meditation Error: LoadProhibited`
(`EXCVADDR: 0x00000000`, típico null/puntero inválido), con este backtrace:

```
LoginBlob::decodeBlob(...) at LoginBlob.cpp:34
LoginBlob::loadZeroconf(...) at LoginBlob.cpp:116
LoginBlob::loadZeroconfQuery(...) at LoginBlob.cpp:198
CSpotConnectPlayer::handlePost(httpd_req*) at cspot_connect.cpp:150
```

**Causa**: la app de Spotify hace el POST de emparejamiento (a
`/spotify_info`) codificado como `application/x-www-form-urlencoded`
(RFC 1866 §8.2.1: `+` es espacio, `%XX` es un byte en hex). El campo
`blob` que manda es base64 — que usa `+`, `/` y `=`, todos caracteres que
el cliente percent-encodea. Mi `handlePost()` (`cspot_connect.cpp`)
partía el body en pares `clave=valor` separados por `&`, pero **nunca
decodificaba nada** — a diferencia del `Shim.cpp` original de
squeezelite-esp32 (no copiado a este componente), que sí llamaba
`url_decode(body)` antes de tokenizar (con `strtok`, que reescribí
justamente para evitar mutar el buffer in-place — y en esa reescritura se
me perdió el paso de decodificar). El resultado: `LoginBlob::decodeBlob`
recibía un `blob` truncado/corrupto, y
`auto encrypted = std::vector<uint8_t>(blob.begin() + 16, blob.end() - 20);`
(`LoginBlob.cpp:34`) queda con un rango de iteradores inválido si el blob
decodificado mide menos de 36 bytes — de ahí el `memcpy` con dirección
nula.

**Corrección aplicada** (`cspot_connect.cpp`): función `urlDecode()`
propia (decodifica `+` y `%XX`, con chequeo de límites y de dígitos hex
válidos antes de convertir, sin usar `strtok` ni mutar nada in-place),
aplicada a la clave y al valor de cada par al armar el `queryMap`. Compila
limpio, verificado.

**Confirmado en hardware real que el fix de F14 estaba bien** (el
`urlDecode` resolvió el primer crash, en `LoginBlob.cpp:34`), pero apareció
un tercer problema — ver F15.

### F15 — `sha1Init()` pasaba `hmac=1` a `mbedtls_md_setup()`, inválido en mbedTLS 4.0 incluso para hash plano (Crítico, encontrado en hardware real, corregido)

Tercer hallazgo con hardware real: con F13 y F14 corregidos, el
emparejamiento llega más lejos — el dispositivo recibe el POST, pero ahora
loguea `LoginBlob.cpp:59: Mac doesn't match!` y después crashea
(`LoadProhibited`) en `decodeBlobSecondary` (`LoginBlob.cpp:105`), en este
bucle:

```cpp
auto l = blobData.size();
for (int i = 0; i < l - 16; i++) {
  blobData[l - i - 1] ^= blobData[l - i - 17];
}
```

Si `blobData` mide menos de 16 bytes, `l - 16` (con `l` de tipo `size_t`,
sin signo) da *underflow* a un número enorme, y el bucle indexa muy lejos
de los límites del vector — de ahí el `LoadProhibited`. Pero eso es sólo el
síntoma final: `blobData` mide menos de 16 bytes porque todo lo que pasó
antes en la cadena de descifrado ya venía roto, empezando por el "Mac
doesn't match" — la verificación de checksum del blob (HMAC-SHA1 sobre los
bytes cifrados, con una clave derivada de `SHA1(sharedKey)`) fallaba.

**Causa real, no relacionada con el DH ni con `urlDecode`**: en el
`Crypto::sha1Init()` portado (`external/bell/main/utilities/Crypto.cpp`),
copiado verbatim del original, la llamada era
`mbedtls_md_setup(&sha1Context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1)`
— con el parámetro `hmac` en `1`. El propio header de mbedTLS 4.0 dice
explícitamente: *"From TF-PSA-Crypto 1.0 and Mbed TLS 4.0 onwards, hmac
MUST be set to 0. HMAC operations are no longer supported via MD"*. El
valor de retorno de `mbedtls_md_setup()` nunca se revisaba (ni en el
original ni en mi copia), así que un fallo ahí pasaba completamente
desapercibido: **cada hash SHA1 "plano" calculado a través de este
contexto salía mal** — y `sha1Init()`/`sha1Update()`/`sha1FinalBytes()` se
usan exactamente así (sin HMAC) en varios puntos de `LoginBlob.cpp`
(`baseKey = SHA1(sharedKey)`, `secret = SHA1(deviceId)`,
`baseKeyHashed = SHA1(pkBaseKey)`) — de ahí que absolutamente todo lo que
depende de esos hashes (checksum, clave de cifrado, y la segunda capa de
descifrado del blob) terminara corrupto.

Este bug **ya estaba en el código original** (pre-mbedTLS 4.0) como un
`hmac=1` innecesario para un uso puramente no-HMAC — probablemente
inofensivo (solo desperdiciaba memoria reservando un contexto HMAC interno
que nunca se usaba) en mbedTLS 2.x/3.x, y se volvió activamente incorrecto
recién con el cambio de contrato en mbedTLS 4.0. Como en mi port
`sha1HMAC()` (el verdadero uso de HMAC) ya no pasa por este contexto en
absoluto — usa `psa_mac_compute()` directamente — el contexto de
`sha1Init()` quedó usado *exclusivamente* para hashing plano, así que
`hmac=0` es lo correcto sin ningún efecto secundario.

**Corrección aplicada**: cambiar ese `1` por `0` en
`external/bell/main/utilities/Crypto.cpp` (único lugar en todo `cspot`/`bell`
donde se llama a `mbedtls_md_setup`, verificado por grep). Compila limpio —
falta que el usuario confirme en la placa que el emparejamiento completa
sin el "Mac doesn't match" ni el crash posterior.

**Confirmado en hardware real que F15 resolvió el emparejamiento**
(`LoginBlob` ya no crashea, el checksum ya no falla). Con eso resuelto
apareció un cuarto problema, esta vez ya no en el emparejamiento sino en la
sesión real con Spotify — ver F16.

### F16 — Cuelgue (watchdog) leyendo la respuesta HTTPS de `apresolve.spotify.com` — sospecha fundada: TLS 1.3, no confirmado aún en hardware (Alto, mitigación aplicada, pendiente de confirmar)

Cuarto hallazgo con hardware real: con F13/F14/F15 corregidos, el
emparejamiento completa y arranca la sesión (`Session::connectWithRandomAp()`
→ `ApResolve::fetchFirstApAddress()`, un GET HTTPS a
`https://apresolve.spotify.com/` para obtener la IP del access point de
Spotify a usar). Ahí se cuelga: el task watchdog dispara repetidamente
(cada 5s) con la tarea `spotifyConnect` siempre en el mismo punto —
`HTTPClient::Response::readResponseHeaders()` → `std::istream::getline()`
→ `SocketBuffer::underflow()`/`xsgetn()` → `TLSSocket::read()`
(`mbedtls_ssl_read`) — bloqueada indefinidamente. La conexión TCP+TLS se
abre sin error (si el handshake fallara, `TLSSocket::open()` tiraría una
excepción antes de llegar acá), pero después ninguna lectura devuelve
datos.

**Hipótesis, no confirmada todavía**: `sdkconfig.defaults` traía
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` — agregado por mí en algún momento
anterior de este proyecto sin una justificación registrada, y **en
contra del default real de ESP-IDF**, que es `n`
(`components/mbedtls/Kconfig`). `HTTPClient.cpp`/`SocketStream.cpp` (código
vendorizado, sin tocar) hacen lectura bloqueante simple, asumiendo que cada
`read()` devuelve datos de aplicación — un patrón común en clientes HTTP
escritos para TLS 1.2. TLS 1.3 introduce mensajes post-handshake
(`NewSessionTicket`) que el servidor puede mandar de forma asíncrona
después del `Finished`; un cliente que no está preparado para eso puede
quedarse esperando una respuesta HTTP que nunca llega "pura", si el primer
dato que cruza el socket es en realidad protocolo TLS y no la respuesta de
la aplicación. Encaja con el síntoma (handshake OK, lectura posterior
colgada), pero **no lo confirmé leyendo un RFC línea por línea ni con una
captura de paquetes** — es la hipótesis mejor fundada que tengo, no una
certeza como F13-F15.

**Mitigación aplicada**: se quitó `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` de
`sdkconfig.defaults`, volviendo al default de ESP-IDF (solo TLS 1.2,
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y` sigue activo). Compila limpio — **esto
todavía no está confirmado en hardware**, a diferencia de los hallazgos
anteriores. Si el cuelgue persiste con este cambio, hay que revisar más a
fondo: posibles candidatos siguientes serían un timeout de lectura ausente
en `mbedtls_net_recv` (bloqueo indefinido genuino si el servidor no
responde por cualquier motivo, no solo por TLS 1.3), o algo específico de
red/DNS hacia `apresolve.spotify.com` desde la red del usuario.

**Confirmado en hardware real: F16 quedó resuelto.** Con logging de
diagnóstico agregado (temporal, en `Crypto.cpp`) se
verificó además que **el emparejamiento completo (F13-F15) funciona
correctamente de punta a punta** — los tres `base64Decode` de la sesión dan
`ok`, la cadena de `sha1HMAC` corre limpia, y ya no aparece "Mac doesn't
match". El GET a `apresolve.spotify.com` también resuelve bien con TLS 1.2.
La sesión avanza hasta intentar conectar con un access point real de
Spotify — ahí apareció un problema distinto, ver F17.

### F17 — `PlainConnection::connect()` se rinde en la primera dirección que falla, en vez de probar las demás (Alto, encontrado en hardware real, corregido)

Quinto hallazgo con hardware real, y el primero que **no tiene nada que
ver con mbedTLS 4.0 ni con este port** — es un bug de lógica en
`src/PlainConnection.cpp` que afectaría a cualquier plataforma.
Con F13-F16 resueltos, la sesión llega a
`Session::connectWithRandomAp() → PlainConnection::connect()`, loguea
`Connecting with AP <ap-guc3.spotify.com:4070>`, y el dispositivo
abortea/reinicia inmediatamente después:

```
abort() was called
--- __cxxabiv1::__terminate
--- __cxa_throw
--- cspot::PlainConnection::connect(...) at PlainConnection.cpp:97
--- cspot::Session::connectWithRandomAp() at Session.cpp:68
```

**Causa**: `PlainConnection.cpp:97` (dentro del `for (ai = airoot; ai; ai =
ai->ai_next)` que recorre las direcciones que devolvió `getaddrinfo()`)
tira `throw std::runtime_error("Can't connect to spotify servers")`
apenas **la primera** dirección falla al conectar — en vez de hacer
`continue` y probar la siguiente. Los hostnames de los access points de
Spotify suelen resolver a más de una IP (balanceo de carga); si la primera
resuelta no es alcanzable por cualquier motivo transitorio, el código
actual se rinde ahí mismo, sin intentar ninguna de las otras. La excepción
además nunca se atrapaba en ningún lado de la cadena de llamada
(`Session::connectWithRandomAp()` → `CSpotConnectPlayer::runSession()` en
`cspot_connect.cpp`), así que terminaba en `std::terminate()`/`abort()` —
tumbando todo el dispositivo por un problema de conectividad transitorio
con una sola IP.

**Corrección aplicada** — dos partes:
1. `src/PlainConnection.cpp`, editado directamente (no relacionado
  con mbedTLS, pero mismo principio de la sección 0: el fix vive en el
  archivo real, no en una copia paralela): la única diferencia real contra
  el original de squeezelite-esp32 es que ahora hace `continue` en vez de
  `throw` dentro del bucle, y sólo tira la excepción **después** del
  bucle, si ninguna dirección funcionó. El resto del archivo
  (`recvPacket`, `sendPrefixPacket`, `readBlock`, `writeBlock`, `close`)
  quedó idéntico al original.
2. `cspot_connect.cpp`: se separó `runSession()` en un wrapper con
  `try/catch (const std::exception&)` alrededor de la lógica real
  (renombrada `runSessionInner()`). cspot tira `std::runtime_error` en
  varios puntos para errores de red/protocolo que espera que el que lo usa
  atrape y decida qué hacer (reconectar, abortar, etc.) — este componente
  no atrapaba nada en ningún lado, así que cualquier error de este tipo
  (no sólo el de `PlainConnection`) tumbaba el dispositivo entero. Ahora se
  loguea y se vuelve a esperar el próximo emparejamiento en vez de
  reiniciar. No es una solución completa (no reintenta la sesión
  automáticamente sin repetir el emparejamiento ZeroConf — ver F10, misma
  limitación de fondo: `bell::Task` no da una forma limpia de manejar esto
  mejor), pero es sustancialmente mejor que un `abort()`.

Compila limpio. **Pendiente de confirmar en hardware** que la sesión ya
completa el `connect()` y avanza más allá de este punto (autenticación,
Shannon cipher, SPIRC).

## 5. Confirmado en hardware real: la sesión de Spotify funciona de punta a punta (el audio físico no — ver F18)

Con F17 corregido, la sesión completa: `PlainConnection` conecta con
`ap-guc3.spotify.com:4070`, el handshake AP (Diffie-Hellman + Shannon
cipher, vía `Crypto::dhCalculateShared`/`sha1HMAC` portados en la sección
2) da `Authorization successful`, el protocolo Mercury se suscribe y
recibe país/tokens, `TrackQueue` resuelve las URLs de CDN y las claves de
audio de cada tema (usando `Crypto::aesCTRXcrypt` portado para
desencriptar), y el reproductor avanza de tema en tema en la cola — logs de
`spotify_example: now playing: Frédéric Chopin - Nocturnes...` con
`TrackPlayer.cpp: Playing`.

**Corrección sobre una afirmación anterior de este documento**: esta
sección decía originalmente que "el dispositivo reproduce audio real"
basándose únicamente en estos logs de protocolo/sesión. Eso fue una
sobre-interpretación — esos logs confirman que el *pipeline* de Spotify
funciona de punta a punta (PCM decodificado y entregado al sink), pero no
que haya sonido saliendo físicamente del parlante. En los hechos, **no
salía audio del parlante** en ningún momento, con o sin el refactor de
`cspot`/Kconfig — ver F18, el bug real detrás de eso, encontrado recién
comparando contra una implementación de referencia para esta misma placa.

Esto confirma correcto, con tráfico real contra los servidores de
Spotify (no solo compilando) todo el port de mbedTLS 4.0 de la sección 2:
el intercambio Diffie-Hellman de 768 bits, la cadena HMAC-SHA1 (`psa_mac_compute`),
y el AES-CTR vía PSA (`psa_cipher_*`) usado para desencriptar el audio.

El logging de diagnóstico agregado para F16/F17 (`Crypto.cpp`) se
**retiró** una vez confirmada la causa —
imprimía claves privadas DH y secretos compartidos en texto plano por el
puerto serie, que no debe quedar en un build de uso normal. Se conservó
como mejora real, no debug, el chequeo del código de retorno de
`mbedtls_base64_decode()` en `base64Decode()` (el original lo ignoraba en
silencio; ahora tira una excepción clara, que el `try/catch` de F17 atrapa
igual que cualquier otro error de sesión).

### F18 — Sin audio físico: el sink I2S usaba `I2S_SLOT_MODE_STEREO`, pero el ampli on-board de la JC3248W535 es mono (Alto, encontrado en hardware real comparando contra una referencia, corregido — pendiente confirmar sonido)

Con F13-F17 resueltos, la sesión de Spotify funciona de punta a punta a
nivel de protocolo (sección 5) — pero **no salía ningún sonido del
parlante**, ni antes ni después del refactor a `cspot`/Kconfig (secciones
0-4): esto es un bug del sink de audio, independiente de todo lo demás.

**Causa**: la JC3248W535 tiene un solo parlante on-board manejado por un
ampli clase D mono (NS4168), sin pin de SD/enable dedicado — el ampli
sigue el reloj I2S directamente. `i2s_audio_sink.cpp`
(`applyStdConfig()`) configuraba el slot mode según el número de canales
que reporta Spotify (`channelCount == 1 ? MONO : STEREO`), y Spotify
siempre decodifica a PCM estéreo — así que el peer quedaba permanentemente
en `I2S_SLOT_MODE_STEREO`. Comparado contra una implementación de
referencia para esta misma placa (funcional, confirmada por el usuario en
otro proyecto), la única diferencia real entre ambas configuraciones de
`i2s_std_config_t` — mismos pines, mismo MCLK, mismo formato Philips,
mismo orden de llamadas `i2s_new_channel`/`i2s_channel_init_std_mode`/
`i2s_channel_enable` — es que la referencia fuerza
`I2S_SLOT_MODE_MONO` de forma fija, más un buffer de datos plano de un
solo canal (no pares LR intercalados) en cada `i2s_channel_write()`.

**Corrección aplicada**: `I2SAudioSink::Config` gana un campo
`monoOutput` (`Kconfig`: `CONFIG_CSPOT_I2S_MONO_OUTPUT`, `bool`, default
`y` para este ejemplo/placa). Cuando está activo:
- `applyStdConfig()` fuerza `I2S_SLOT_MODE_MONO` sin importar el
  `channelCount` que reporte Spotify.
- `feedPCMFrames()` hace *downmix* de las muestras estéreo intercaladas
  (`(L+R)/2`, con acumulador de 32 bits para no desbordar) a un buffer
  plano de un solo canal antes de escribirlo — coincidiendo con el formato
  de buffer que espera el modo mono del driver I2S de ESP-IDF (un sample
  por frame, no un par LR).

Se puede desactivar (`monoOutput = false`) para un DAC estéreo real
(PCM5102/UDA1334-style) en la misma placa de pines.

Compila limpio (`idf.py build`, 0 errores). **Actualización tras probar en
hardware**: el silencio total resultó ser, en los hechos, un **conector
del parlante suelto/mal contactado** en la placa del usuario — no un bug
de software. El downmix a mono descrito arriba sigue siendo la
configuración correcta para este ampli mono (y quedó aplicado), pero no
era la causa del síntoma original "no sale nada de audio". Una vez
resuelto el conector, el audio sí se escucha — con un artefacto periódico
nuevo, ver F20.

### F19 — Stack overflow en la tarea `mercury_dispatcher` al reconectar tras un error de red (Alto, encontrado en hardware real, corregido)

Sexto hallazgo con hardware real: con varios temas reproducidos con
éxito, después de ~8 minutos de sesión aparece un error de lectura de
socket, seguido de un crash inmediato:

```
MercurySession.cpp:53: Error while receiving packet: Error in read
PlainConnection.cpp:219: Closing socket...
***ERROR*** A stack overflow in task mercury_dispatc has been detected.
```

**Causa**: `MercurySession::MercurySession()` (`MercurySession.cpp:25`)
crea su propia tarea (`bell::Task("mercury_dispatcher", 4 * 1024, 3, 1)`)
con **4 KB de stack** — mucho menos que sus tareas hermanas
(`CSpotTrackQueue` usa 32 KB, `cspot_player` 48 KB). En el camino feliz
(recibir y despachar paquetes Mercury) 4 KB alcanza. Pero
`MercurySession::runTask()` atrapa cualquier `std::runtime_error` de
`recvPacket()`/`sendPacket()` (como el "Error in read" de arriba, que
`PlainConnection::readBlock()` tira tras varios reintentos fallidos) y
llama a `reconnect()` — que sobre la misma pila de 4 KB: (1) vuelve a
correr `Session::connectWithRandomAp()` → `ApResolve::fetchFirstApAddress()`
(`ApResolve.cpp`), un GET HTTPS completo vía `bell::HTTPClient` (handshake
TLS entero) seguido de un `nlohmann::json::parse()` (parser recursivo,
consumo de pila no trivial por nivel de anidamiento); y (2) llama a
`Session::authenticate()` (`Session.cpp:73`), que declara
`APWelcome welcome;` **como variable local de pila** — un struct nanopb de
~1 KB (arrastra `reusable_auth_credentials` de 512 bytes,
`lfs_secret` de 128 bytes, `canonical_username` de 30 bytes, más
`AccountInfo`/`AccountInfoFacebook` anidados — ver
`protobuf/authentication.options`). Ese único struct ya es ~25-30% del
stack total de la tarea, apilado además sobre los propios frames de
`runTask()`/`reconnect()` y el handshake TLS/parseo JSON — desborda los
4 KB con facilidad.

**Corrección aplicada**: subir el stack de `mercury_dispatcher` de 4 KB a
16 KB (`MercurySession.cpp`). Corre sobre PSRAM (comportamiento por
defecto de `bell::Task`, ver `BellTask.h`/finding F4), que sobra en esta
placa — no hay motivo para mantenerlo ajustado, a diferencia de la RAM
interna. Compila limpio. **No confirmado en hardware todavía** que el
desborde específico ya no ocurra (haría falta reproducir varios minutos
de sesión con un error de red real de por medio para confirmarlo con
certeza) — pero la causa está identificada con precisión (el stack
disponible antes del fix, ~4 KB, es literalmente menor que un solo struct
local de la ruta de reconexión).

### F20 — Audio con "rayones" periódicos (glitch cada pocos segundos): buffer DMA de I2S minúsculo + power-save de Wi-Fi activo (Alto, encontrado en hardware real, corregido — pendiente confirmar en hardware)

Séptimo hallazgo con hardware real: con F18 resuelto (el silencio era un
conector suelto, no software), el audio sí suena, pero con un artefacto
periódico — un fragmento limpio, luego una distorsión tipo "prrrrrrr",
repitiéndose aproximadamente cada 3 segundos. Patrón típico de un
*buffer underrun*: el consumidor (periférico I2S) se queda sin datos
nuevos antes de que el productor (red + decodificador Vorbis) le entregue
el siguiente bloque.

**Causa, dos factores combinados**:
1. `i2s_audio_sink.cpp` creaba el canal I2S con
   `I2S_CHANNEL_DEFAULT_CONFIG`, que trae `dma_desc_num=6`,
   `dma_frame_num=240` — sólo 1440 frames en total, **~33ms de buffer** a
   44.1kHz. Cualquier pausa del productor por encima de eso (jitter de
   red trayendo el audio desde el CDN de Spotify, tiempo de decodificación
   de `tremor`, latencia del driver Wi-Fi) vacía el buffer al instante y
   se escucha como un corte.
2. Este proyecto **nunca llamaba a `esp_wifi_set_ps()`**, así que el
   radio Wi-Fi quedaba en el power-save por defecto de ESP-IDF
   (`WIFI_PS_MIN_MODEM`): duerme el radio entre balizas DTIM del AP,
   introduciendo picos de latencia periódicos (típicamente 100ms o más,
   en el orden de la baliza/DTIM del punto de acceso) — encaja con la
   periodicidad de "cada pocos segundos" del síntoma, y es la causa
   probable más específica del *patrón regular*, mientras que el buffer
   chico es lo que hace que cualquier pico, sea cual sea su origen, se
   escuche como un corte.

**Corrección aplicada** — ambas partes:
1. `i2s_audio_sink.cpp` (`I2SAudioSink::I2SAudioSink()`): sube
   `dma_desc_num` a 10 y `dma_frame_num` a 1000 (10 000 frames, ~227ms de
   colchón a 44.1kHz mono, ~113ms en estéreo) — 1000 frames/descriptor se
   mantiene bajo el límite de hardware de ~4092 bytes por buffer DMA
   incluso en estéreo de 16 bits (4000 bytes).
2. `main.cpp` (`connectWifi()`): `esp_wifi_set_ps(WIFI_PS_NONE)` después
   de `esp_wifi_start()` — desactiva el power-save por completo. Trade-off
   consciente: más consumo, pero para un parlante alimentado por la red
   (no a batería) es la elección correcta frente a audio con cortes.

Compila limpio (`idf.py build`, 0 errores). **No confirmado en hardware
todavía** que el glitch desaparezca por completo — si persiste con menor
frecuencia/intensidad, el siguiente paso sería aumentar aún más el buffer
DMA (el techo real es la RAM interna disponible, no PSRAM — los buffers
DMA de I2S no pueden vivir en PSRAM en esta configuración) y/o revisar si
`CDNAudioFile`/`HTTPClient` (código de `cspot`, sin tocar) hace lecturas
de red bloqueantes de forma que puedan introducir pausas largas por sí
mismas, independientes del Wi-Fi power-save.

**Lo que queda pendiente, no bloqueante para el uso normal**:
- F2 (verificación de certificado TLS deshabilitada) — resuelto en ESP32
  por F55 (sección 8); compila y entra en la partición, pero todavía no
  está confirmado en hardware real (no se probó el handshake TLS contra
  los servidores reales de Spotify con la verificación activada).
- F10 (`cspot_connect_stop()` no libera memoria de forma segura) y la
  falta de reintento automático de sesión sin repetir el emparejamiento
  ZeroConf tras un error de red (ver F17) — quedan como casos de borde no
  probados en esta sesión de pruebas.
- F19, F20 — pendientes de confirmación en hardware real (ver arriba). F18
  quedó confirmado (era un problema de conector, no de software).

## 6. Auditoría de código adicional (julio 2026): bugs, mejoras y refactorings

Con el componente ya funcionando de punta a punta en hardware real (F1-F20),
esto es una pasada de revisión de código deliberada — no motivada por un
síntoma concreto — buscando bugs, huecos de robustez y oportunidades de
refactoring que el ciclo compilar→probar→corregir de las secciones
anteriores no necesariamente iba a sacar a la luz.

**Alcance**: revisión propia, línea por línea, de todo el código que
mantenemos directamente — `cspot_connect.{h,cpp}`, `i2s_audio_sink.{h,cpp}`,
`CMakeLists.txt`, `Kconfig`, `main.cpp` — más los archivos de `cspot/bell`
donde ya aplicamos cambios propios (`Crypto.{h,cpp}`, `TLSSocket.{h,cpp}`,
`X509Bundle.cpp`, `PlainConnection.cpp`, `MercurySession.cpp`). Además,
delegué a un agente una revisión de los archivos "core" de sesión que
nunca tocamos pero que están directamente en el camino crítico de
emparejamiento/autenticación/reproducción — `Session.cpp`, `LoginBlob.cpp`,
`AuthChallenges.cpp`, `TrackQueue.cpp`, `TrackPlayer.cpp`,
`CDNAudioFile.cpp`, `SpircHandler.cpp`, `ApResolve.cpp`,
`AccessKeyFetcher.cpp`. Los hallazgos de esa segunda pasada (F21-F23,
F25-F27, F36) los verifiqué yo mismo leyendo el código fuente antes de
documentarlos acá, no los tomé de la palabra del agente. **No es una
auditoría exhaustiva de las ~150 fuentes vendorizadas de `cspot`/`bell`**
— se priorizó lo que está en el camino crítico de este producto
(emparejamiento, autenticación, sesión, audio), no todo el árbol.

La mayoría de estos hallazgos ya está corregida (ver estado por fila en
la tabla); los que quedan pendientes se documentan igual para decidir en
qué orden abordarlos. Para las dos rondas de trabajo posteriores a esta
auditoría (hardening continuo en hardware real, y la implementación
completa de la UI con display), ver la **sección 9**.

### Resumen

| ID | Severidad | Estado | Archivo | Qué pasa |
|---|---|---|---|---|
| F21 | Crítico | ✅ Corregido | `AuthChallenges.cpp` | Desborde de buffer heap con datos de red sin autenticar (ZeroConf) |
| F22 | Crítico | ✅ Corregido | `LoginBlob.cpp` | Offsets/tamaños del blob de emparejamiento sin validar contra el tamaño real |
| F23 | Alto | ✅ Corregido | `AuthChallenges.cpp` | Asume ≥4 bytes en la respuesta AP-Hello sin chequear |
| F24 | Alto | ✅ Corregido | `SpircHandler.cpp` | Deref de `shared_ptr` nulo si la cola se vacía justo al notificar playback |
| F25 | Alto | ✅ Corregido | `TrackQueue.cpp` | Acceso fuera de rango a un `deque` vacío, disparable desde un frame SPIRC remoto |
| F26 | Alto | ✅ Corregido | `TrackQueue.cpp` / `AccessKeyFetcher.cpp` | Excepción de parseo JSON sin atrapar en una tarea distinta a la de F17 → reinicio |
| F27 | Alto | ✅ Corregido | `CDNAudioFile.cpp` | Underflow sin signo calculando el tamaño del archivo, asignación gigante |
| F28 | Alto | ✅ Corregido | `PlainConnection.cpp` | `getaddrinfo()` fallido usa/libera un puntero sin inicializar |
| F29 | Alto | ✅ Corregido | `TLSSocket.cpp` | Fallo de `mbedtls_net_connect()` no aborta, sigue al handshake TLS igual |
| F30 | Alto | ✅ Corregido | `TLSSocket.cpp` / `BellSocket.h` | `read()`/`write()` devuelven `size_t` pero mbedTLS puede devolver códigos negativos |
| F31 | Alto | ✅ Corregido | `cspot_connect.cpp` | `cspot_connect_stop()` no despierta la tarea si está esperando un pairing |
| F32 | Medio | ✅ Corregido | `MercurySession.cpp` | `reconnect()` recursivo sin límite, crece la pila en cada reintento |
| F33 | Medio | Pendiente | `PlainConnection.cpp` | `EINTR` mal manejado en `readBlock`/`writeBlock`, puede cortar una lectura en silencio |
| F34 | Medio | ✅ Corregido | `PlainConnection.cpp` | `recvPacket()` confía en el tamaño de paquete de la red sin validarlo |
| F35 | Medio | ✅ Corregido | `LoginBlob.cpp` | Mismatch de HMAC solo se loguea, no aborta el descifrado |
| F36 | Medio | ✅ Corregido | `ApResolve.cpp` | `ap_list[0]` sin chequear que exista, depende del catch genérico de F17 |
| F37 | Medio | ✅ Corregido | `Crypto.cpp` | Fuga del contexto SHA1 si una secuencia `sha1Init`→`sha1FinalBytes` se interrumpe |
| F38 | Medio | ✅ Corregido | `i2s_audio_sink.cpp` | El downmix a mono solo aplica con `bitDepth==16`, pero el slot mode no tiene esa misma condición |
| F39 | Medio | Pendiente | `i2s_audio_sink.cpp` | `setParams()` actualiza el estado interno antes de confirmar que el hardware se reconfiguró |
| F40 | Medio | Pendiente | `MercurySession.cpp` | `catch (...)` vacío silencia fallos de envío, con un `@TODO` sin resolver |
| F41 | Medio | Pendiente | `Kconfig` / `Kconfig.projbuild` | Sin validar que credenciales de Spotify/Wi-Fi se hayan configurado |
| F42 | Medio | Pendiente | `main.cpp` | Reconexión de Wi-Fi sin backoff ante fallos persistentes |
| F43 | Medio | Pendiente | `cspot_connect.cpp` | Falla de autenticación no dispara `CSPOT_EVENT_DISCONNECTED` |
| F44 | Bajo | Pendiente | `CMakeLists.txt` | Flags de warning con prefijo `-Wno-` inconsistente |
| F45 | Bajo | ✅ Corregido | `CMakeLists.txt` | `PRIV_REQUIRES` de más (`nvs_flash`, `esp_wifi`, `esp_netif`) |
| F46 | Bajo | ✅ Corregido | `Crypto.cpp` | Patrón `psa_import_key`/`psa_destroy_key` repetido, candidato a RAII |
| F47 | Bajo | ✅ Corregido | `main.cpp` | Comentario desactualizado ("DAC estéreo") |
| F48 | Bajo | ✅ Corregido | `CMakeLists.txt` | `add_definitions()` es API de CMake obsoleta |
| F49 | Bajo | Pendiente | `i2s_audio_sink.cpp` | Muestra sobrante silenciosa si el conteo de samples es impar |
| F50 | Bajo | Pendiente | `cspot_connect.cpp` | `CSpotConnectPlayer` hace demasiadas cosas en una sola clase |

21 de los 30 hallazgos (F21-F32, F34-F38, F45-F48) están corregidos
y verificados con `idf.py build` (0 errores) — ver el detalle de cada
corrección en su propia entrada más abajo. Los 9 restantes quedan
documentados sin aplicar.

### Crítico / Alto

#### F21 — `AuthChallenges::prepareAuthPacket()` desborda un array fijo de 512 bytes con datos que vienen de una petición HTTP sin autenticar (Crítico, ✅ corregido)

`AuthChallenges.cpp:37-39`:

```cpp
std::copy(authData.begin(), authData.end(),
          authRequest.login_credentials.auth_data.bytes);
authRequest.login_credentials.auth_data.size = authData.size();
```

`auth_data` es un campo nanopb generado con `max_size:512`
(`protobuf/authentication.options:2`) — es decir, un array `uint8_t[512]`
fijo embebido en el struct `authRequest` (miembro de `AuthChallenges`).
`std::copy` no chequea el tamaño de destino: si `authData.size() > 512`,
escribe más allá del array, corrompiendo memoria del heap adyacente al
objeto `AuthChallenges`.

**Encadenado con F22**, esto es alcanzable sin autenticación desde
cualquier dispositivo en la red local: `authData` es `LoginBlob::authData`,
poblado en `LoginBlob::loadZeroconf()` a partir del campo `blob` del POST
a `/spotify_info` (nuestro propio `handlePost()` en `cspot_connect.cpp`,
el endpoint de emparejamiento ZeroConf — sin ninguna autenticación previa,
por diseño, ya que es el primer paso del pairing). `Session::authenticate()`
(`Session.cpp:77`) pasa `blob->authData` directo a `prepareAuthPacket()`.
Un blob de emparejamiento malformado (o simplemente corrupto por un error
de red) puede terminar corrompiendo memoria del dispositivo antes de que
exista ninguna sesión autenticada con Spotify.

**Corrección aplicada**: validar `authData.size() <= sizeof(auth_data.bytes)`
antes del `std::copy` (o `std::min`-clampear + loguear/abortar si excede),
en `prepareAuthPacket()`. Complementar con F22 (validar `authSize` contra
el tamaño real del blob decodificado antes de construir `authData`).

#### F22 — `LoginBlob::readBlobInt()`/`loadZeroconf()` no validan offsets ni tamaños contra los datos reales (Crítico, ✅ corregido)

`LoginBlob.cpp:69-80`:

```cpp
uint32_t LoginBlob::readBlobInt(const std::vector<uint8_t>& data) {
  auto lo = data[blobSkipPosition];
  ...
  auto hi = data[blobSkipPosition + 1];
```

Indexa `data[blobSkipPosition]`/`data[blobSkipPosition + 1]` sin chequear
`blobSkipPosition` contra `data.size()`. Y en `loadZeroconf()`
(`LoginBlob.cpp:125-129`):

```cpp
auto authSize = readBlobInt(loginData);
...
this->authData = std::vector<uint8_t>(
    loginData.begin() + blobSkipPosition,
    loginData.begin() + blobSkipPosition + authSize);
```

`authSize` sale del propio blob decodificado (dato de red, aunque pasado
por descifrado AES primero) y nunca se valida contra
`loginData.size() - blobSkipPosition`. Un blob truncado o corrupto
produce un rango de iteradores inválido — comportamiento indefinido, muy
probablemente un crash — **antes incluso de llegar al desborde de F21**.
Mismo vector de entrada: el POST sin autenticar a `/spotify_info`.

**Corrección aplicada**: en `readBlobInt()`, chequear
`blobSkipPosition < data.size()` (y `+1 < data.size()` para el caso de dos
bytes) antes de indexar, lanzando o devolviendo un valor de error. En
`loadZeroconf()`, verificar `blobSkipPosition + authSize <= loginData.size()`
antes de construir `authData`.

#### F23 — `AuthChallenges::solveApHello()` asume que la respuesta del AP mide al menos 4 bytes (Alto, ✅ corregido)

`AuthChallenges.cpp:59`:

```cpp
auto skipSize = std::vector<uint8_t>(data.begin() + 4, data.end());
```

`data` viene de `conn->recvPacket()` — un paquete de red real, del lado
del Access Point de Spotify (no del usuario, pero sí una dependencia
externa fuera de nuestro control). Si por cualquier motivo la respuesta
mide menos de 4 bytes, `data.begin() + 4` ya es un iterador inválido antes
de construir el rango — comportamiento indefinido. Baja probabilidad
(sería un bug/comportamiento inesperado del propio servidor de Spotify),
pero el costo de chequearlo es una línea.

**Corrección aplicada**: `if (data.size() < 4) throw std::runtime_error(...)`
al principio de la función.

#### F24 — `SpircHandler::notifyAudioReachedPlayback()` puede desreferenciar un `shared_ptr` nulo (Alto, ✅ corregido)

`SpircHandler.cpp:92-112`. `trackQueue->consumeTrack(nullptr, offset)`
devuelve `nullptr` cuando la cola está agotada (confirmado en
`TrackQueue.cpp:447-448/475-477`, y `TrackPlayer.cpp:147` sí lo chequea en
ese otro call site) — pero acá el resultado se usa sin chequear null en
ambas ramas (`->requestedPosition`, `->trackInfo`). Si la cola se vacía
justo cuando se dispara esta notificación (última pista terminando, o un
`skipTrack(NEXT)` que falla), es un deref nulo → crash.

**Corrección aplicada**: se chequea el puntero devuelto por
`consumeTrack()` antes de usarlo, en los dos call sites de la función
(el inicial y el que se re-adquiere tras `skipTrack()`) — si es
`nullptr`, la función retorna sin notificar, igual de espíritu que
`TrackPlayer.cpp:147`. Verificado con `idf.py build`, 0 errores.

#### F25 — `TrackQueue::updateTracks()` accede a un `deque` vacío sin chequear (Alto, ✅ corregido)

`TrackQueue.cpp:615`: `else if (preloadedTracks[0]->loading)` — sin
`.empty()` antes, cuando `initial == false`. Se llega acá desde
`SpircHandler::handleFrame()` en `MessageType_kMessageTypeReplace`
(`SpircHandler.cpp:209`) — un frame SPIRC que manda el cliente remoto
(el teléfono/app controlando la reproducción). Si llega un frame REPLACE
antes de que algún LOAD haya poblado `preloadedTracks` (o después de que
haya quedado vacío por otro motivo), es acceso fuera de rango sobre un
`deque` vacío.

**Corrección aplicada**: `if (!preloadedTracks.empty() && preloadedTracks[0]->loading)`.
Verificado con `idf.py build`, 0 errores.

#### F26 — Una excepción de parseo JSON en la tarea de `TrackQueue` no la atrapa nada — reinicia el dispositivo, independiente del fix de F17 (Alto, ✅ corregido)

`TrackQueue::runTask()` (`TrackQueue.cpp:414`) llama a
`AccessKeyFetcher::updateAccessKey()` (`AccessKeyFetcher.cpp:88-92`). En
la rama que realmente compila este proyecto (`BELL_ONLY_CJSON`), el bug
concreto no era una excepción (cJSON no tira) sino un **deref nulo**:
`cJSON_GetObjectItem(root, "access_token")->valuestring` sin chequear que
`root` (si `cJSON_Parse` falla) ni el item mismo fueran no-nulos. En la
rama `nlohmann` (no compilada acá) sí era una excepción real de
`nlohmann::json::parse`/acceso a claves inexistentes. Una respuesta
malformada/truncada (por ejemplo, una página de error de un proxy en el
medio en vez del JSON de token esperado) disparaba esto. **`TrackQueue`
corre en su propia `bell::Task` ("CSpotTrackQueue"), una tarea/pila
distinta a la que envuelve el `try/catch` de `runSession()` documentado en
F17** — nada atrapaba esto, terminando en `std::terminate()`/`abort()`
(rama nlohmann) o un crash directo por puntero nulo (rama cJSON),
reiniciando el dispositivo. El fix de F17 solo protege la tarea principal
de sesión, no esta.

**Corrección aplicada**: en vez de solo envolver la llamada en un
`try/catch` (que no habría evitado el deref nulo de la rama cJSON, ya que
ahí no hay excepción que atrapar), `AccessKeyFetcher::updateAccessKey()`
se reescribió para validar explícitamente antes de usar los datos: en la
rama `cJSON`, chequeo de `root`/`access_token`/`expires_in`/`valuestring`
no nulos antes de leerlos; en la rama `nlohmann`, el `parse()` y los
accesos quedaron dentro de su propio `try/catch` con log del error. En
ambos casos, una respuesta malformada ahora cae en la rama de "fallo,
reintentar" que ya existía (log + `BELL_SLEEP_MS(3000)` + reintento hasta
3 veces) en vez de crashear. Verificado con `idf.py build`, 0 errores.

#### F27 — `CDNAudioFile::openStream()` puede hacer un underflow sin signo calculando el tamaño del archivo (Alto, ✅ corregido)

`CDNAudioFile.cpp:51-62`:
`totalFileSize = httpConnection->totalLength() - SPOTIFY_OPUS_HEADER` —
si el `Content-Length` que declara el CDN es menor que el tamaño de
cabecera esperado (respuesta HTTP corta/truncada), esta resta con
`size_t` da un número gigante en vez de negativo. Ese valor gigante
alimenta después el tamaño de un `std::vector<uint8_t>` para el footer —
intento de asignar varios exabytes, que tira `bad_alloc`/aborta (mejor
caso) o corrompe los límites de un `memcpy` posterior en `readBytes` (peor
caso).

**Corrección aplicada**: chequear
`httpConnection->totalLength() >= SPOTIFY_OPUS_HEADER` antes de restar, y
tratar un archivo más chico que la cabecera esperada como error de
streaming.

#### F28 — `PlainConnection::connect()`: si `getaddrinfo()` falla, se usa (y se libera) un puntero sin inicializar (Alto, ✅ corregido)

`PlainConnection.cpp:59-61`:

```cpp
if (getaddrinfo(hostname.c_str(), portStr.c_str(), &h, &airoot)) {
  CSPOT_LOG(error, "getaddrinfo failed");
}
```

Solo loguea — no hace `return`/`throw`. `airoot` (`struct addrinfo*`, sin
inicializar en su declaración) queda con un valor indeterminado si
`getaddrinfo()` falla (POSIX no garantiza que lo toque en ese caso). La
ejecución sigue directo al `for (ai = airoot; ...)` con ese puntero
potencialmente basura, y más abajo a `freeaddrinfo(airoot)` — liberar un
puntero no inicializado es comportamiento indefinido, con buenas chances
de crash o corrupción de heap. Un fallo de resolución DNS transitorio
(exactamente el tipo de condición que F17 se ocupó de manejar mejor un
paso más adelante en esta misma función) dispararía esto.

**Corrección aplicada**: `if (getaddrinfo(...)) { throw std::runtime_error("getaddrinfo failed"); }`
— inicializar `airoot = nullptr` en la declaración como defensa adicional.

#### F29 — `TLSSocket::open()`: si `mbedtls_net_connect()` falla, no aborta — sigue armando el handshake TLS sobre un socket roto (Alto, ✅ corregido)

`TLSSocket.cpp:30-34`:

```cpp
if ((ret = mbedtls_net_connect(&server_fd, hostUrl.c_str(), ...)) != 0) {
  BELL_LOG(error, "http_tls", "failed! connect returned %d\n", ret);
}
```

Es el único chequeo de error de las cuatro llamadas en esta función que
NO tira excepción — `mbedtls_ssl_config_defaults()`,
`mbedtls_ssl_set_hostname()` y el `while (mbedtls_ssl_handshake(...))` sí
lo hacen, lo cual sugiere que esto es un descuido y no una decisión
deliberada. Si el TCP connect falla (DNS resuelve pero el host rechaza la
conexión, timeout, red caída), el código sigue igual hacia
`mbedtls_ssl_setup()`/`mbedtls_ssl_handshake()` sobre un `server_fd`
inválido — en vez de un error claro de "no pude conectar", el fallo
aparece más adelante como un error de bajo nivel de lectura/escritura TLS,
más difícil de diagnosticar (el mismo tipo de síntoma confuso que costó
horas en la investigación de F16).

**Corrección aplicada**: `throw std::runtime_error("mbedtls_net_connect failed")`
en vez de solo loguear, igual que las otras tres validaciones de esta
misma función.

#### F30 — La interfaz `Socket::read()`/`write()` declara `size_t`, pero mbedTLS puede devolver códigos de error negativos (Alto, de diseño, ✅ corregido)

`BellSocket.h:13-14` declara `virtual size_t write(...)`/
`virtual size_t read(...)`. `TLSSocket::read()`/`write()`
(`TLSSocket.cpp:70-76`) devuelven directo `mbedtls_ssl_read()`/
`mbedtls_ssl_write()`, que son `int` y pueden devolver valores negativos
(`MBEDTLS_ERR_SSL_WANT_READ`, `MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY`, etc.).
Un `int` negativo convertido implícitamente a `size_t` (sin signo) se
convierte en un número enorme. El único consumidor que revisé
(`SocketStream.cpp:50,82`) lo vuelve a asignar a una variable `ssize_t`
(con signo) — en la práctica esto "funciona" en un compilador con
complemento a dos porque el patrón de bits hace un round-trip correcto,
pero es tácito, no garantizado por el estándar antes de C++20, y
cualquier código nuevo que use el valor de retorno como `size_t` genuino
(por ejemplo, para dimensionar un buffer) se rompería con un valor
absurdo. No es algo que introdujimos — es un hueco en el diseño de la
interfaz `Socket` de upstream — pero ahora que `TLSSocket.cpp` es código
que mantenemos directamente, vale la pena endurecerlo.

**Corrección aplicada**: en `TLSSocket::read()`/`write()`, chequear si el
valor de mbedTLS es negativo y devolver `0` (o agregar un mecanismo de
error explícito), en vez de dejar que el bit pattern se reinterprete
tácitamente aguas abajo.

#### F31 — `cspot_connect_stop()` no puede despertar la tarea si está esperando un emparejamiento (Alto, ✅ corregido)

`cspot_connect.cpp`, `runTask()`:

```cpp
while (running) {
  clientConnected.wait();
  if (!running) break;
  ...
}
```

`requestStop()` (llamado por `cspot_connect_stop()`) solo hace
`running = false;`. Si la tarea está bloqueada en
`clientConnected.wait()` — que es el estado normal de espera, sin ningún
cliente Spotify conectado todavía — nada la despierta para que vuelva a
evaluar `running`. La tarea queda dormida para siempre, aunque
`cspot_connect_stop()` ya haya "vuelto". Esto agrava lo que F10 ya
documenta como fuga intencional: ni siquiera llega a tener la chance de
salir prolijamente. `bell::WrappedSemaphore` sí expone `give()`
públicamente (`WrappedSemaphore.h:35`), así que el fix es trivial.

**Corrección aplicada**: `requestStop()` ahora llama
`clientConnected.give()` además de `running = false;` — el `wait()`
se desbloquea, el loop re-evalúa `running`, ve que es `false`, y sale
por el `break` antes de intentar `runSession()`. Verificado con
`idf.py build`, 0 errores.

**Corrección sugerida**: `void requestStop() { running = false; clientConnected.give(); }`.

### Medio

#### F32 — `MercurySession::reconnect()` es recursivo sin límite — el fix de stack de F19 puede no alcanzar en una caída de red larga (Medio, ✅ corregido)

`MercurySession.cpp:99-106`:

```cpp
} catch (...) {
  CSPOT_LOG(error, "Cannot reconnect, will retry in 5s");
  BELL_SLEEP_MS(5000);
  if (isRunning) {
    return reconnect();
  }
}
```

Cada intento fallido vuelve a llamar a `reconnect()` recursivamente, no en
un loop. No es necesariamente tail-call-optimizable (está dentro de un
bloque `catch`, lo que normalmente inhibe esa optimización en la mayoría
de los compiladores). El aumento de stack a 16KB de F19 le da mucho más
margen al camino de reconexión normal, pero una caída de red
suficientemente larga (varios minutos, reintentando cada 5s) sigue
pudiendo acumular suficientes frames como para volver a desbordar,
aunque haga falta mucho más tiempo para llegar ahí que antes del fix.

**Corrección aplicada**: convertir la recursión en un `while (isRunning)`
con el mismo cuerpo — stack constante sin importar cuántos reintentos
hagan falta.

#### F33 — `PlainConnection::readBlock()`/`writeBlock()`: el caso `EINTR` puede cortar una lectura en silencio por underflow de un índice sin signo (Medio)

`PlainConnection.cpp:161-181`. Cuando `recv()` devuelve `<= 0`, el switch
sobre `getErrno()` tiene `case EINTR: break;` — eso sale del `switch`,
pero cae directo a `idx += n;` con `n` todavía en 0 o negativo (el valor
que devolvió el `recv()` fallido). Con `idx` de tipo `unsigned int`, sumar
un `n` negativo hace underflow: si `idx` era 0 (interrupción en la
primera lectura), `idx` pasa a valer `UINT_MAX`, y la condición
`while (idx < size)` se vuelve falsa inmediatamente — la función retorna
como si hubiera leído todo, dejando el buffer del caller sin llenar
(parcial o totalmente). En este target (lwIP/FreeRTOS) `EINTR` es poco
común porque no hay señales POSIX reales, así que la probabilidad de
disparar esto en la práctica es baja — pero es un bug de lógica real,
heredado de upstream, no algo que introdujimos.

**Corrección sugerida**: en el caso `EINTR`, usar `goto READ`/`goto WRITE`
igual que los otros casos que deben reintentar, en vez de `break` (que
deja caer la ejecución al `idx += n` incorrecto).

#### F34 — `PlainConnection::recvPacket()` confía en el tamaño de paquete que manda la red, sin validarlo (Medio, ✅ corregido)

`PlainConnection.cpp:126-138`:

```cpp
uint32_t packetSize = ntohl(extract<uint32_t>(packetBuffer, 0));
packetBuffer.resize(packetSize, 0);
readBlock(packetBuffer.data() + 4, packetSize - 4);
```

Si `packetSize < 4` (paquete corrupto/malformado), `packetSize - 4`
(sin signo) da un número gigante, y `readBlock()` intenta leer una
cantidad de bytes imposible — se queda bloqueado indefinidamente (o
falla tras varios reintentos con timeout, según el patrón de F17/F33).
Además, cualquier `packetSize` grande-pero-corrupto dispara un
`resize()` que puede agotar el heap. Esta es la fase **previa** al
handshake Diffie-Hellman (`PlainConnection`, no `ShannonConnection`), así
que en principio no está cifrada — un dispositivo malicioso en la misma
red local haciéndose pasar por (o interponiéndose entre el cliente y) un
Access Point de Spotify podría mandar un tamaño de paquete corrupto a
propósito.

**Corrección aplicada**: validar `packetSize >= 4` y un techo razonable
(por ejemplo, unos pocos MB) antes de `resize()`/`readBlock()`.

#### F35 — `LoginBlob::decodeBlob()`: un HMAC que no coincide solo se loguea, no aborta el descifrado (Medio, ✅ corregido)

`LoginBlob.cpp:58-60`:

```cpp
if (mac != checksum) {
  CSPOT_LOG(error, "Mac doesn't match!");
}
```

No hay `return`/`throw` — la función sigue igual hacia
`crypto->aesCTRXcrypt(...)` y devuelve datos descifrados con una clave
potencialmente equivocada. Con una clave compartida DH stale/repetida (un
segundo intento de pairing tras uno fallido, por ejemplo) esto produce
"datos" que en realidad son ruido, que terminan alimentando
`decodeBlobSecondary()`/`readBlobInt()` — exactamente el tipo de entrada
que dispara F21/F22. Esto ya causó un crash real documentado como F15 (con
una causa raíz distinta, el `hmac=1` de mbedTLS 4.0) — pero incluso con
F15 corregido, un mismatch de MAC legítimo (clave realmente equivocada,
no un bug de mbedTLS) sigue sin frenar nada acá.

**Corrección aplicada**: `if (mac != checksum) { throw std::runtime_error("blob checksum mismatch"); }`
(además del log existente). Verificado con `idf.py build`, 0 errores.

#### F36 — `ApResolve::fetchFirstApAddress()`: `ap_list[0]` sin chequear que la lista no esté vacía (Medio, ✅ corregido)

`ApResolve.cpp:39-40`. Si el JSON de respuesta trae `"ap_list": []`,
`nlohmann::json`'s `operator[]` no-const en un array vacío inserta un
`null` en la posición 0 en vez de tirar, y la conversión implícita de ese
`null` a `std::string` sí tira — así que técnicamente no crashea sin
control (lo atrapa el `try/catch` genérico de F17), pero como error es
opaco: el log solo va a decir "session failed: [json exception message]",
sin pista de que el problema real fue una lista de APs vacía.

**Corrección aplicada**: en la rama `BELL_ONLY_CJSON` (la que compila este
proyecto), chequeo explícito de `cJSON_GetObjectItem`/`cJSON_GetArrayItem`/
`valuestring` no nulos antes de usarlos, con `throw std::runtime_error(...)`
y mensaje claro si falta algo. En la rama `nlohmann` (no compilada acá,
pero mantenida por paridad con el resto del archivo), chequeo equivalente
con `.contains()`/`.is_array()`/`.empty()`. Verificado con `idf.py build`,
0 errores.

#### F37 — `CryptoMbedTLS`: el contexto SHA1 puede filtrarse si una secuencia `sha1Init()`→`sha1FinalBytes()` se interrumpe (Medio, ✅ corregido)

`Crypto.cpp:82-87` — `sha1Init()` llama `mbedtls_md_init(&sha1Context)` +
`mbedtls_md_setup(...)` cada vez que se invoca, sin llamar primero a
`mbedtls_md_free()` sobre lo que hubiera quedado de una secuencia previa.
`mbedtls_md_setup()` asigna estado interno propio del algoritmo; si un
`sha1Init()`→`sha1Update()` anterior nunca llegó a
`sha1FinalBytes()`/`sha1Final()` (las únicas dos funciones que llaman
`mbedtls_md_free()`) — por ejemplo, porque algo lanzó una excepción en el
medio de una cadena como la de `LoginBlob::decodeBlobSecondary()`, que
mezcla `sha1Init()`/`sha1Update()` con `pbkdf2HmacSha1()`/
`aesECBdecrypt()`, ambas capaces de tirar — esa asignación anterior queda
huérfana. El destructor de `CryptoMbedTLS` (`Crypto.cpp:27`) tampoco la
libera (está vacío). Como `LoginBlob` guarda una única instancia de
`Crypto` de por vida (no una nueva por intento de pairing), esto puede
acumularse en escenarios con varios intentos de emparejamiento/reconexión
fallidos.

**Corrección aplicada**: `sha1Init()` ahora llama
`mbedtls_md_free(&sha1Context)` al principio, antes de
`mbedtls_md_init()`, liberando cualquier asignación huérfana de una
secuencia anterior interrumpida. Para que eso sea seguro incluso en la
*primera* llamada (cuando `sha1Context` todavía es memoria sin
inicializar, ya que el constructor de `CryptoMbedTLS` no tocaba el
miembro), el constructor ahora llama `mbedtls_md_init(&sha1Context)` para
dejarlo en un estado conocido desde el principio. El destructor
(antes vacío) ahora llama `mbedtls_md_free(&sha1Context)` como red de
seguridad adicional. Verificado con `idf.py build`, 0 errores.

#### F38 — El downmix a mono no tiene la misma condición de `bitDepth` que la selección de slot mode del I2S (Medio, no alcanzable hoy, ✅ corregido)

`i2s_audio_sink.cpp`, `applyStdConfig()` pone `I2S_SLOT_MODE_MONO` sin
condición sobre `bitDepth` cuando `config.monoOutput` está activo, pero
`feedPCMFrames()` solo hace el downmix `if (bitDepth == 16 && channelCount == 2 && config.monoOutput)`.
Hoy esto nunca se dispara porque Spotify siempre manda 16 bits
(`cspot_connect.cpp`: `audioSink->setParams(44100, 2, 16)`), pero si
alguna vez cambiara (otro codec, otra tasa de bits), el periférico I2S
quedaría configurado en MONO mientras `feedPCMFrames()` seguiría mandando
el buffer estéreo intercalado sin downmixear — audio corrupto, sin ningún
error visible.

**Corrección aplicada**: unificar la condición — o el downmix soporta
cualquier `bitDepth` (generalizando el bucle a 8/16/24/32 bits), o
`applyStdConfig()` cae a estéreo real cuando `bitDepth != 16` aunque
`monoOutput` esté activo, con un log de advertencia.

#### F39 — `I2SAudioSink::setParams()` actualiza el estado antes de confirmar que el hardware se reconfiguró (Medio, difícil de disparar)

`i2s_audio_sink.cpp:62-84`. `sampleRate`/`channelCount`/`bitDepth` se
asignan a los valores nuevos ANTES de llamar `i2s_channel_disable()` +
`applyStdConfig()`. Si `i2s_channel_disable()` falla con un código
distinto de `ESP_ERR_INVALID_STATE`, la función devuelve `false` sin
haber llegado a `applyStdConfig()` — pero los miembros ya quedaron con
los valores nuevos, aunque el hardware siga en el formato viejo. Cualquier
llamada posterior a `feedPCMFrames()` calcularía el downmix/volumen según
el formato equivocado. Requiere que `i2s_channel_disable()` falle de una
forma inusual para disparar, así que es más una inconsistencia latente
que un bug observado.

**Corrección sugerida**: mover las asignaciones de
`sampleRate`/`channelCount`/`bitDepth` después de que
`i2s_channel_enable()` confirme éxito, o revertirlas explícitamente en el
camino de error.

#### F40 — `MercurySession::executeSubscription()`/`requestAudioKey()`: fallos de envío se tragaban en silencio (Medio, ✅ logging agregado)

`MercurySession.cpp` (`executeSubscription()`, `requestAudioKey()`), antes:

```cpp
} catch (...) {
  // @TODO: handle disconnect
}
```

Si `shanConn->sendPacket()` tira (siempre `std::runtime_error`, verificado
siguiendo la cadena `ShannonConnection::sendPacket()` →
`PlainConnection::writeBlock()`), la excepción se descartaba sin loguear
nada. Quien hizo la petición/suscripción nunca se enteraba. Es un `@TODO`
dejado por upstream, no algo nuestro. El caso de `executeSubscription()`
es el más serio: `SpircHandler::subscribeToMercury()` lo usa para
suscribirse a `hm://remote/user/.../`, el canal por el que llegan **todos**
los comandos remotos — si ese envío falla en silencio, la sesión queda
sorda a comandos remotos sin ningún indicio en el log. El caso de
`requestAudioKey()` es menos grave: el track que lo espera ya tiene un
timeout aguas arriba (`TrackPlayer.cpp`'s `track->loadedSemaphore->
twait(5000)`), así que hoy en día resulta en un stall silencioso de 5s,
no un cuelgue permanente.

**Por qué no se resolvió el pendiente de la propia respuesta (notificar el
callback ahí mismo, o disparar `reconnect()`)**: se investigó y se
descartó por riesgo real de introducir un bug peor. `requestAudioKey()`
se llama desde `TrackQueue::processTrack()` con `tracksMutex` ya tomado
(`TrackQueue.cpp:450-457`), y el callback que recibiría la notificación de
fallo vuelve a tomar ese mismo mutex (`TrackQueue.cpp:281`) — resolverlo
sincrónicamente en el `catch` sería un deadlock (mutex no recursivo,
mismo hilo). Mismo tipo de riesgo que F62 ya evitó por falta de
visibilidad completa del código interno de `TrackQueue`. Disparar
`reconnect()` desde acá tampoco es seguro: `executeSubscription()` corre
en el hilo de sesión, no en el propio de `MercurySession::runTask()`
("mercury_dispatcher"), y `reconnect()` toca `this->conn`/`this->shanConn`
sin ningún mutex, asumiendo que solo su propio `runTask()` lo llama.

**Corrección aplicada**: se agregó logging (`CSPOT_LOG(error, ...)`,
incluyendo la URI/motivo) a ambos `catch`, sin tocar el resto del
comportamiento — el pendiente sigue sin resolverse por sí mismo (se
apoya en el timeout externo ya existente para `requestAudioKey()`, o en
que un error de lectura no relacionado dispare `failAllPending()` para
`executeSubscription()`), pero ahora es diagnosticable en vez de
completamente invisible.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre).

#### F41 — Nada valida que las credenciales de Spotify/Wi-Fi se hayan configurado antes de intentar usarlas (Medio)

`CONFIG_CSPOT_CLIENT_ID`/`CONFIG_CSPOT_CLIENT_SECRET` (`Kconfig`) tienen
`default ""`, y `CONFIG_EXAMPLE_WIFI_SSID`/`CONFIG_EXAMPLE_WIFI_PASSWORD`
(`main/Kconfig.projbuild`) tienen defaults literales
(`"myssid"`/`"mypassword"`) — ninguno de los dos casos se valida en
tiempo de ejecución. Si alguien compila sin pasar por `idf.py menuconfig`
primero, el síntoma es indirecto y confuso: Wi-Fi queda reintentando para
siempre en silencio (ver F42) si el SSID es el placeholder, o la sesión
de Spotify falla con un genérico "authentication failed" si las
credenciales están vacías — nada dice explícitamente "te olvidaste de
configurar X".

**Corrección sugerida**: en `cspot_connect_start()`, chequear
`strlen(CONFIG_CSPOT_CLIENT_ID) == 0` y devolver `false` con un
`ESP_LOGE` explícito. En `main.cpp`, un chequeo equivalente para el SSID
placeholder antes de `connectWifi()`.

#### F42 — Reconexión de Wi-Fi sin backoff (Medio)

`main.cpp`, `wifiEventHandler()`:
`case WIFI_EVENT_STA_DISCONNECTED: esp_wifi_connect();` — reintenta
inmediatamente, sin demora ni límite de intentos. Ante una contraseña
incorrecta o un AP caído, esto reintenta en un loop apretado
indefinidamente. Patrón común en ejemplos de ESP-IDF, no exclusivo de
este proyecto, pero vale la pena una demora/backoff dado que este
firmware está pensado para correr como aparato de uso continuo.

**Corrección sugerida**: un `BELL_SLEEP_MS`/`vTaskDelay` corto (por
ejemplo, 2-5s) antes de reintentar, con backoff exponencial opcional y un
tope razonable.

#### F43 — Una autenticación fallida no dispara `CSPOT_EVENT_DISCONNECTED` (Medio)

`cspot_connect.cpp`, `runSessionInner()`:

```cpp
if (ctx->config.authData.empty()) {
  ESP_LOGE(TAG, "authentication failed, waiting for a new pairing attempt");
  return;
}
```

A diferencia del camino de desconexión normal (que sí llama
`notify(CSPOT_EVENT_DISCONNECTED, nullptr)` antes de retornar), este
`return` temprano no notifica nada. Un consumidor de la API que use
`event_cb` para llevar su propio estado de conexión (por ejemplo, una UI
mostrando "conectando...") se queda sin ninguna señal de que este intento
en particular falló.

**Corrección sugerida**: agregar el mismo `notify(CSPOT_EVENT_DISCONNECTED, nullptr);`
antes de este `return`.

### Bajo (calidad de código / refactorings)

#### F44 — Flags de warning con prefijo `-Wno-` inconsistente en `CMakeLists.txt`

`components/CMakeLists.txt (repo root):14-16`:

```cmake
add_definitions(-Wno-unused-variable -Wno-unused-const-variable
                 -Wchar-subscripts -Wunused-label -Wmaybe-uninitialized
                 -Wmisleading-indentation)
```

Los primeros dos flags sí tienen el prefijo `-Wno-` (los suprimen); los
otros cuatro no lo tienen — o sea que en vez de silenciarlos, los está
habilitando explícitamente (probablemente ya lo estaban vía `-Wall`, así
que es un no-op en la práctica). Dado que F1c señala por nombre
`-Wmaybe-uninitialized`/`-Wmisleading-indentation` como ruidosos en
código vendorizado, agrupar estos cuatro junto a los dos que sí se
suprimen sugiere que la intención era suprimir los seis. Si esa era la
intención, hoy no se está cumpliendo para el código propio de este
componente (`cspot_connect.cpp`/`i2s_audio_sink.cpp`).

**Corrección sugerida**: agregar `-Wno-` a los cuatro flags restantes, o
confirmar que la falta de `-Wno-` fue intencional y borrar el comentario
que sugiere lo contrario.

#### F45 — `PRIV_REQUIRES` de más en `CMakeLists.txt` (✅ Corregido)

`nvs_flash`, `esp_wifi` y `esp_netif` están declarados como
`PRIV_REQUIRES` de este componente, pero ni `cspot_connect.cpp` ni
`i2s_audio_sink.cpp` usan ningún símbolo de esos tres (confirmado por
grep) — los necesita `main/`, que ya los declara por su cuenta. No rompe
nada (ESP-IDF tolera dependencias de más), es solo ruido/cruft que
probablemente sobrevivió de una versión anterior de `cspot_connect.cpp`
que inicializaba Wi-Fi/NVS internamente.

**Corrección aplicada**: sacar los tres de `PRIV_REQUIRES`, recompilar
para confirmar que sigue linkeando limpio.

#### F46 — Patrón `psa_import_key`/`psa_destroy_key` repetido en `Crypto.cpp` (✅ Corregido)

`sha1HMAC()` y `aesCTRXcrypt()` repetían el mismo patrón: crear
`psa_key_attributes_t`, importar, operar, destruir la key. Cosmético — no
corregía ningún bug activo (con el código anterior, `psa_destroy_key` sí
se llegaba a llamar en todos los caminos existentes) — pero cualquier
cuarto uso de PSA agregado a este archivo en el futuro podía olvidarse
del `destroy` en algún camino de error nuevo.

**Corrección aplicada**: clase `PsaKeyHandle` (namespace anónimo, arriba
en `Crypto.cpp`) — el constructor hace `psa_import_key()` y tira si
falla, el destructor hace `psa_destroy_key()` incondicionalmente al salir
de scope (retorno normal o excepción). `sha1HMAC()` y `aesCTRXcrypt()`
ahora la usan en vez de manejar `mbedtls_svc_key_id_t` a mano. Verificado
con `idf.py build`, 0 errores.

#### F47 — Comentario desactualizado en `main.cpp` (✅ Corregido)

El comentario de cabecera todavía decía "Audio goes out over I2S to a
plain stereo DAC (PCM5102/UDA1334-style board)" — desactualizado desde
que el downmix a mono (`CONFIG_CSPOT_I2S_MONO_OUTPUT`, activado por
defecto) se volvió el target real de este ejemplo (JC3248W535).

**Corrección aplicada**: comentario actualizado para mencionar el
downmix a mono como default y cómo desactivarlo para un DAC estéreo
real.

#### F48 — `add_definitions()` es una API de CMake heredada (✅ Corregido)

`components/CMakeLists.txt (repo root)` usaba `add_definitions()`, que CMake
considera legado desde hace años en favor de
`add_compile_options()`/`target_compile_options()` — era el único uso en
todo el proyecto (verificado por grep contra el `CMakeLists.txt` raíz y
`main/CMakeLists.txt`). Sin diferencia funcional; modernización de estilo.

**Corrección aplicada**: cambiado a `add_compile_options()`, mismos
flags. Verificado con `idf.py build`, 0 errores.

#### F49 — Muestra sobrante silenciosa si el conteo de samples es impar

`I2SAudioSink::feedPCMFrames()`: si `bytes` no es múltiplo de 2 (o, en el
camino de downmix, si `sampleCount` es impar), la última muestra suelta
se descarta sin ningún log ni assert. Improbable en la práctica (cspot
siempre entrega PCM estéreo intercalado en cantidades pares de samples),
pero un `ESP_LOGW` cuando se detecta ayudaría a diagnosticar si alguna
vez pasa.

#### F50 — `CSpotConnectPlayer` concentra demasiadas responsabilidades

Una sola clase maneja: servidor HTTP + registro mDNS + ciclo de vida de
sesión + traducción de eventos cspot → `cspot_event_t`. Funciona bien
hoy, pero separar el servidor de emparejamiento ZeroConf (`handleGet`/
`handlePost`/`startHttpServerAndMdns`) en su propia clase mejoraría la
legibilidad y permitiría testear esa parte de forma aislada. Sugerencia
de diseño, no una corrección de nada roto.

## 7. F51 — Port de `bell/main/audio-sinks/esp` a `driver/i2s_std.h`: se retira `i2s_audio_sink.cpp` propio (Refactor, ✅ aplicado y confirmado en hardware)

Distinto en naturaleza a los hallazgos anteriores: no es un bug encontrado,
es un refactor pedido explícitamente — llevar a `bell/main/audio-sinks/esp`
(el directorio donde F1 documentó que todos los sinks de audio de ESP32
usaban el driver I2S legacy, `driver/i2s.h`, removido en ESP-IDF v5.4+/v6)
el mismo tipo de port a `driver/i2s_std.h` que ya habíamos hecho por
separado en nuestro propio `i2s_audio_sink.cpp`, para poder desistir de
ese archivo propio y usar directamente la clase de `bell`.

**Alcance**: de los 8 sinks ESP32 que trae `bell` (`AC101`, `BufferedAudioSink`
— la base compartida, no un sink en sí —, `ES8311`, `ES8388`, `ES9018`,
`InternalAudioSink`, `PCM5102`, `SPDIF`, `TAS5711`), solo se portaron **dos**:

- **`BufferedAudioSink`** — la base compartida de la que heredan todos los
  demás: dueña del periférico I2S (antes `i2s_driver_install`/`i2s_write`/
  `i2s_set_clk`, ahora `i2s_new_channel`/`i2s_channel_write`/
  `i2s_channel_init_std_mode`, igual que nuestro `i2s_audio_sink.cpp`
  retirado), más el ring buffer + tarea dedicada (`i2sFeed`) que ya traía
  de antes — sin tocar, salvo la llamada a escritura I2S en sí. Ahora
  también implementa acá, compartido por cualquier sink que herede de
  esta clase: escalado de volumen por software (lo que F6 documentaba como
  faltante en *todos* los sinks vendorizados) y el downmix a mono opcional
  (`Config::monoOutput`, para amplificadores mono de un solo parlante como
  el NS4168 de la JC3248W535), con la misma unificación de condición
  `bitDepth`/slot mode que F38 corrigió en nuestro propio sink.
- **`PCM5102AudioSink`** — el caso "DAC I2S plano sin interfaz de control"
  (BCLK/WS/DOUT(+MCLK), nada de I2C). El nombre es histórico — no tiene
  nada específico del chip PCM5102, es el sink genérico. Antes tenía los
  pines fijos en el código (`bck_io_num=27, ws_io_num=32,
  data_out_num=25`, hallazgo F9); ahora toma un `Config` con la misma
  forma que tenía nuestro `I2SAudioSink::Config` (`port`, `bclkPin`,
  `wsPin`, `doutPin`, `mclkPin`, `monoOutput`).

**No se tocaron** (siguen en `driver/i2s.h`, y por lo tanto excluidos del
build — ver más abajo): `AC101AudioSink`, `ES8311AudioSink`,
`ES8388AudioSink`, `ES9018AudioSink`, `InternalAudioSink`,
`SPDIFAudioSink`, `TAS5711AudioSink`, y sus drivers C de control por I2C
(`ac101.c`, `es8311.c`). Portar esos implica además escribir/verificar la
inicialización del códec por I2C de cada chip, sin hardware para probar
ninguno — fuera de alcance de este pase; quedan como trabajo futuro si
alguna vez hace falta un DAC/ampli con códec real en vez de un DAC plano.

**Cambios de build**:
- `components/CMakeLists.txt (repo root)`: se saca `set(BELL_DISABLE_SINKS ON)`
  — los sinks de `bell` ya no están deshabilitados en bloque.
- `external/bell/CMakeLists.txt`: el glob que arma `SINK_SOURCES` ahora
  excluye por `list(FILTER ... EXCLUDE REGEX ...)` los archivos de los
  sinks no portados (mismo patrón que ya se usaba para excluir
  AACDecoder/OPUSDecoder, ver F1b), en vez de deshabilitar el directorio
  entero.
- `bell` es un target de CMake plano (via `add_subdirectory`, no un
  componente ESP-IDF registrado con `idf_component_register`), así que no
  hereda automáticamente el include-path injection de ESP-IDF — hubo que
  enlazarlo explícitamente contra `idf::esp_driver_i2s` (para
  `driver/i2s_std.h`) e `idf::esp_ringbuf` (para `freertos/ringbuf.h`,
  que `BufferedAudioSink` ya usaba de antes) en
  `components/CMakeLists.txt (repo root)`. `cspot_connect.cpp` en sí también
  necesitó `esp_ringbuf` agregado a su propio `PRIV_REQUIRES`, al incluir
  `PCM5102AudioSink.h` transitivamente.
- `components/cspot/i2s_audio_sink.{h,cpp}` e
  `components/include/i2s_audio_sink.h` — **eliminados**.
  `cspot_connect.cpp` ahora incluye `PCM5102AudioSink.h` y construye un
  `PCM5102AudioSink` en vez de nuestro propio `I2SAudioSink`; el miembro
  `audioSink` pasó a ser `std::unique_ptr<AudioSink>` (el puntero a la
  clase base) en vez de apuntar al tipo concreto, ya que solo se usa a
  través de la interfaz `AudioSink` (`feedPCMFrames`/`setParams`/
  `volumeChanged`).

**Diferencia de arquitectura a tener en cuenta**: nuestro sink retirado
escribía de forma síncrona y bloqueante (`i2s_channel_write(...,
portMAX_DELAY)` directo desde `feedPCMFrames()`, en la propia tarea de
sesión de cspot). El de `bell` desacopla eso: `feedPCMFrames()` solo
empuja bytes a un ring buffer (por defecto 32 KB, `xRingbufferCreate`,
sale del heap general — con PSRAM habilitada en este proyecto, probablemente
ahí termina viviendo), y una tarea dedicada (`i2sFeedTask`, prioridad 10,
4 KB de stack en RAM interna) es la que efectivamente escribe al
periférico I2S. En los hechos, esto agrega una capa extra de colchón
*antes* del buffer DMA de I2S (que ya tiene el tamaño ampliado de F20) —
en principio debería ayudar más todavía con el glitch periódico de F20,
no empeorarlo, pero es un cambio real de arquitectura, no un simple
cambio de nombre de archivo.

Compila limpio (`idf.py build`, 0 errores, mismo 3% de margen de
partición aproximadamente). **Confirmado en hardware real** — el audio
suena correctamente a través de `PCM5102AudioSink`/`BufferedAudioSink`
portadas, con el `i2s_audio_sink.cpp` propio ya retirado.

### F52 — Pausar desde el cliente de Spotify no detiene el audio (Alto, encontrado en hardware real, corregido y confirmado en hardware)

Séptimo hallazgo con hardware real (numeración continúa desde la sección
6): apretar pausa en la app de Spotify se ve reflejado en los logs —
`SpircHandler.cpp:301: External pause command`, y nuestro propio
`cspot_example: playback: pause` — pero **el audio sigue sonando** sin
cortarse.

**Causa**: `cspot::TrackPlayer` no tiene ningún mecanismo de pausa en
vivo. `SpircHandler::setPause()` (`SpircHandler.cpp:299-310`) solo
actualiza `playbackState` (el estado que se reporta de vuelta a los
servidores de Spotify, para que la UI de la app muestre el ícono
correcto) y dispara el evento `PLAY_PAUSE` — pero el loop de
decodificación/entrega de `TrackPlayer::runTask()`
(`TrackPlayer.cpp:208-252`) no consulta `playbackState` en ningún
momento; sigue leyendo Ogg Vorbis y llamando al `dataCallback` sin
importar el estado de pausa. Es responsabilidad de la integración (este
componente) actuar sobre el evento — cosa que `cspot_connect.cpp` no
hacía: su manejo de `EventType::PLAY_PAUSE` solo llamaba `notify(...)`
(el callback de la app, que loguea "playback: pause") sin tocar el sink
de audio para nada. Mismo patrón que F6 (volumen): cspot expone estado/
eventos, pero espera que la integración actúe sobre ellos.

**Corrección aplicada**: nuevo miembro `std::atomic<bool> paused{false}`
en `CSpotConnectPlayer`. El handler de `PLAY_PAUSE` lo actualiza además
de notificar. `pcmWrite()` (el `dataCallback` que le pasamos a
`TrackPlayer`) ahora chequea `paused` al principio y, si está en pausa,
devuelve `0` **sin** llamar a `audioSink->feedPCMFrames()`. El `0` es
deliberado y no arbitrario: `TrackPlayer.cpp:245-248` interpreta un
retorno de `0` como "todavía no pude escribir este chunk, reintentá" —
duerme 50ms y vuelve a ofrecer el mismo bloque de PCM en vez de
descartarlo y avanzar — así que al despausar, la reproducción continúa
exactamente donde se cortó, en vez de saltearse el audio que sonó
"de más" mientras estaba pausado.

**Limitación conocida, corregida más tarde en F77**: el `BufferedAudioSink`
de F51 mete una capa de buffering considerable entre `feedPCMFrames()` y
el parlante (ring buffer de 32 KB + buffer DMA de I2S ampliado por F20) —
al pausar, lo que ya estaba en esos buffers se seguía reproduciendo hasta
vaciarse antes de que el silencio fuera perceptible (hasta ~600ms en el
peor caso con los buffers llenos). No se implementó en su momento ningún
mecanismo para vaciarlos, ya que `BufferedAudioSink` no exponía una API de
"flush" — ver F77 para la corrección.

Compila limpio (`idf.py build`, 0 errores). **Confirmado en hardware,
parcialmente**: pausar efectivamente detiene el avance de la pista y
despausar retoma con normalidad — el mecanismo en sí (el `paused`
atómico + el retorno de `0` en `pcmWrite()`) funciona como se esperaba.
Lo que no funcionaba bien era la consecuencia de dejar de escribir al
sink: en vez de silencio, se escuchaba un efecto tipo "disco rayado" —
causa distinta, ver F53.

### F53 — Al pausar, se escucha un efecto "disco rayado" en vez de silencio: underrun del DMA de I2S sin `auto_clear` (Alto, encontrado en hardware real, corregido)

Encontrado probando F52 en hardware: pausar sí corta el avance de la
pista (F52 funciona), pero en vez de silencio se escucha un sonido
repetitivo tipo disco rayado hasta que se despausa.

**Causa**: al pausar, `pcmWrite()` deja de escribir datos nuevos al
`BufferedAudioSink` (F52) — pero lo que ya estaba en tránsito (el ring
buffer de 32 KB y, sobre todo, el buffer DMA de I2S de F20/F51) se sigue
reproduciendo hasta vaciarse, y ahí es donde aparece el problema: el
periférico I2S de ESP-IDF, al quedarse sin datos nuevos para un buffer
DMA, por defecto **no manda silencio — repite en loop el contenido del
último buffer DMA transmitido**. Con un buffer de ~1000 frames por
descriptor (`dma_frame_num`, ver F20), esa repetición en loop de un
fragmento de audio de milisegundos es exactamente lo que se percibe como
"disco rayado".

`i2s_chan_config_t` (`driver/i2s_common.h`) expone justo el flag para
esto: `auto_clear`/`auto_clear_after_cb` — *"Set to auto clear DMA TX
buffer after `on_sent` callback, I2S will always send zero automatically
if no data to send"*. `BufferedAudioSink::initI2sChannel()`
(`external/bell/main/audio-sinks/esp/BufferedAudioSink.cpp`, agregado en
F51) nunca lo seteaba — quedó en `false`, el default de
`I2S_CHANNEL_DEFAULT_CONFIG`. Dato interesante: el driver legacy
(`driver/i2s.h`) que reemplazamos en F51 **sí** tenía el equivalente
seteado — el `PCM5102AudioSink.cpp` original (antes del port) traía
`.tx_desc_auto_clear = true` con el comentario `// Auto clear tx
descriptor on underflow` — se perdió al portar a la API nueva, sin que
nadie lo hubiera notado hasta pausar de verdad en hardware (compilar y
hasta reproducir música seguida nunca lo iba a mostrar, solo un underrun
real como el de pausa lo dispara).

**Corrección aplicada**: `chanConfig.auto_clear = true;` en
`BufferedAudioSink::initI2sChannel()`, junto a `dma_desc_num`/
`dma_frame_num`. Compila limpio (`idf.py build`, 0 errores). **No
confirmado en hardware todavía** — hace falta flashear y confirmar que
pausar ahora da silencio real en vez del efecto de disco rayado.

### F54 — `BufferedAudioSink::feedPCMFrames()`: escalado de volumen + downmix a mono fusionados en una sola pasada (Eficiencia, aplicado)

No es un bug — una optimización pedida explícitamente tras revisar
oportunidades de reducir consumo de CPU/memoria del proyecto en general.
Cuando el volumen no está al máximo (`volumeScale < 1`) *y* el downmix a
mono está activo (`Config::monoOutput`, ver F38/F51) — el caso normal en
la JC3248W535 en cuanto se baja el volumen desde la app — el código
anterior hacía **dos pasadas completas** sobre el buffer PCM: una para
escalar el volumen y escribir el resultado en `scaleBuffer` (mismo
tamaño que la entrada), y otra para leer `scaleBuffer` y promediar pares
L/R en `downmixBuffer` (la mitad del tamaño) — la primera pasada era
puro trabajo desperdiciado en cuanto la segunda la iba a leer completa
inmediatamente después.

**Corrección aplicada**: cuando ambas transformaciones aplican a la vez,
`feedPCMFrames()` ahora hace una sola pasada — escala y clampea cada
canal (L y R) directo desde el buffer de entrada, y promedia el par en
el mismo paso, escribiendo directo a `downmixBuffer` sin pasar por
`scaleBuffer`. La aritmética por sample es idéntica a la versión de dos
pasadas (mismo orden de escalado→clamp→promedio entero), así que el
resultado es bit a bit el mismo — el cambio es puramente de cuántas
veces se recorre el buffer, no de qué valores produce. Los casos donde
solo aplica una de las dos transformaciones (o ninguna) quedaron sin
tocar, ya eran de una sola pasada.

Compila limpio (`idf.py build`, 0 errores). No requiere confirmación en
hardware más allá de que el audio siga sonando igual (el resultado
numérico es idéntico por diseño) — no es una corrección de comportamiento
observable, solo de trabajo hecho de más.

## 8. F55 — Verificación real de certificados TLS (`X509Bundle`), portada por plataforma con `esp_crt_bundle_attach()` (Seguridad, aplicado — pendiente confirmar en hardware)

Distinto en espíritu a todo lo anterior: no es un bug de este proyecto ni
un pedido de eficiencia — es cerrar el hallazgo de seguridad F2
(verificación de certificado TLS deshabilitada), documentado en detalle
y explicado desde cero en [`aprendizaje.md`](aprendizaje.md#2026-07-13--seguridad-tls-verificación-de-certificados-y-mbedtls)
(entrada "Seguridad: TLS, verificación de certificados y mbedTLS") —
este finding es el resumen técnico de lo que ahí se explica con más
contexto.

**Diagnóstico previo (F2, section 2)**: `X509Bundle::shouldVerify()`
devolvía `false` hardcodeado — heredado de upstream
(`X509Bundle::init()` nunca se llamaba en ningún lugar de
`squeezelite-esp32` original), no algo introducido por el port a
mbedTLS 4.0. Con eso, toda conexión HTTPS de este componente (login5,
access-token, CDN) aceptaba cualquier certificado sin validarlo —
vulnerable a MITM para cualquiera ya presente en la misma red local.

**Diseño aplicado** — tres decisiones, no solo "portar la función vieja":

1. **`X509Bundle` pasa a tener una implementación por plataforma**, seso
  siguiendo el mismo patrón que `bell` ya usa para `MDNSService`/
  `WrappedSemaphore` (`bell/main/platform/{esp,linux,apple,win32}/`), en
  vez de ser un único archivo compartido por las cuatro plataformas de
  `bell` (que es lo que hacía que estuviera "apagado para todos" sin que
  nadie lo decidiera explícitamente). `bell/main/io/X509Bundle.cpp`
  (el stub compartido) se eliminó.
2. **La implementación de ESP32** (`bell/main/platform/esp/X509Bundle.cpp`)
  delega la verificación entera a `esp_crt_bundle_attach()` — el bundle
  de CAs raíz que ESP-IDF mantiene activamente
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`) — en vez de portar a mano
  `crtCheckCertificate()`/`crtVerifyCallback()` (que hubiera significado
  mapear algoritmos de firma legacy a identificadores PSA, código de
  seguridad real escrito desde cero y sin el escrutinio que tiene el
  bundle de Espressif). Ninguna de esas dos funciones se llama desde
  ningún otro lado (verificado por grep), así que se dejaron sin definir,
  igual que hacía el stub que reemplazan.
3. **Linux/Apple/Windows** (`bell/main/platform/{linux,apple,win32}/X509Bundle.cpp`)
  quedaron con el mismo stub "no verifica nada" que había antes — sin
  regresión de comportamiento para esas plataformas (que este proyecto no
  compila ni prueba), solo se preserva que el árbol siga siendo
  compilable ahí.
4. **La decisión de verificar o no es ahora una opción de Kconfig
  explícita** — `CONFIG_CSPOT_TLS_VERIFY_CERTIFICATES` (nuevo, en
  `components/cspot/Kconfig`), **`default y`** (fail-safe: verificado
  salvo que alguien lo desactive a propósito, al revés del `false`
  hardcodeado anterior). `select`s `MBEDTLS_CERTIFICATE_BUNDLE` de
  ESP-IDF para asegurar que el bundle esté disponible cuando esta opción
  está activa.

**Costo real, medido**: habilitar esto agrega **~20 KB de flash**
(el bundle de CAs + el código de verificación), incluso usando la
variante "común" del bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y`,
agregada a `sdkconfig.defaults` — ~50% más chica que la variante completa
por defecto de ESP-IDF, ~99% de cobertura según la propia ayuda de ese
Kconfig) en vez de la completa. El margen de la partición, que venía en
3%, **bajó a 1%** (de ~40 000 a ~19 872 bytes libres) con la tabla de
particiones de ese momento — resuelto ampliando la partición de la app en
vez de recortar funcionalidad, ver **F56**.

Compila limpio (`idf.py build`, 0 errores, ver el costo de flash arriba).
**No confirmado en hardware todavía** — hace falta flashear y confirmar
que el handshake TLS sigue completando correctamente contra los
certificados reales de Spotify (si el bundle no tuviera la CA correcta,
o si `esp_crt_bundle_attach()` fallara por algún motivo, las conexiones
HTTPS empezarían a fallar en vez de silenciosamente aceptar cualquier
cosa — comportamiento esperado y deseado, pero que solo se confirma
probando contra los servidores reales).

## 9. Auditoría de código adicional y hardening continuo en hardware real (F56-F74, julio 2026)

Continuación de la auditoría de la sección 6, en dos etapas posteriores,
ambas verificadas iterativamente contra hardware real. A partir de esta
sección, los hallazgos se documentan en **dos** documentos según su
alcance (numeración F compartida entre ambos, sin renumerar):

- **Este documento** cubre el componente vendorizado `cspot`/`bell` en
  sí — su código, findings, refactorings y fixes.
- [`app_arquitectura.md`](app_arquitectura.md) cubre "nuestra" app: la UI
  (`components/ui/`), el BSP de la placa, `main/`, y decisiones de
  Kconfig/memoria de todo el sistema.

Resumen de las dos etapas:

- **F56-F60**: cabos sueltos de F55 (margen de flash) y hallazgos de las
  últimas pruebas de audio-solo (sin display) — tabla de particiones a
  medida (app), un problema de configuración (no de código) en
  `AccessKeyFetcher` (componente), un bug real de reconexión al CDN
  (componente), una aclaración sobre el keepalive de la sesión
  (componente), y soporte de MP3 para podcasts (componente).
- **F61-F74**: implementación completa de la UI con display (pantallas de
  WiFi setup y reproducción sobre `bsp_jc3248w535`/LVGL, app) y la cadena
  de depuración en hardware que hizo falta para estabilizarla —
  reconfiguración de flash/PSRAM/heap (app), tres crashes sin try/catch
  (componente), cuatro rondas de ajuste de memoria (mitad app, mitad
  componente), un stack overflow (app), una UI que se congelaba por un
  mutex mal usado (app), y el mecanismo de posición de reproducción en
  tiempo real (mitad componente: exponer `getPositionMs()`; mitad app: su
  propia saga de prioridades/cores de FreeRTOS). Todo confirmado
  funcionando de punta a punta en la prueba final.

### Resumen (hallazgos físicamente en este documento)

| ID | Severidad | Estado | Archivo | Qué pasa |
|---|---|---|---|---|
| F57 | Medio | ✅ Confirmado (no era bug) | `Kconfig`/`sdkconfig` | `AccessKeyFetcher` fallaba por `CSPOT_CLIENT_SECRET` vacío, no un bug de código |
| F58 | Alto | ✅ Corregido, confirmado | `TLSSocket.cpp` | Silencio total al reanudar tras pausa larga: conexión al CDN muerta sin timeout de lectura |
| F59 | Informativo | ✅ Confirmado | `MercurySession.cpp` | Aclaración: la sesión ya tiene keepalive y reconexión propios, no era un bug |
| F60 | Media/Alto | Aplicado, pendiente confirmar | `TrackQueue.cpp` | Soporte de audio MP3 para episodios de podcast |
| F62 | Alto | ✅ Confirmado en hardware | `cspot_connect.{h,cpp}` | `play/pause/next/previous()`: extensión de control local sobre `SpircHandler` (la UI que lo consume vive en `app_arquitectura.md`) |
| F63 | Alto | ✅ Corregido, confirmado | `cspot_connect.cpp` | Reinicio al refrescar el token: `HTTPClient::post()` sin try/catch |
| F64 | Alto | ✅ Corregido, confirmado | `sdkconfig.defaults` | `AccessKeyFetcher` no lograba conectar: buffers de mbedTLS sin lugar en DRAM interna |
| F65 | Alto | ✅ Corregido, confirmado | `cspot_connect.cpp` | Segundo crash sin relación: `handlePost()` (pairing ZeroConf) sin try/catch |
| F67 | Alto | ✅ Aplicado, confirmado | `sdkconfig.defaults` | Contención del AES por hardware + pool interno fijo de LVGL |
| F71 | Medio | ✅ Aplicado, confirmado | `SpircHandler.{h,cpp}`, `cspot_connect.{h,cpp}` | `getPositionMs()`: posición de reproducción real (SPIRC) expuesta como API (el consumo en la UI vive en `app_arquitectura.md`) |

**F56, F61, F66, F68, F70, F72-F74** son código/decisiones de la app, no
del componente — ver [`app_arquitectura.md`](app_arquitectura.md) para su
contenido completo.

Nota: **F69** no tiene entrada propia — ese hallazgo (bloqueo de los
botones por el mismo mutex que F70) se documentó directamente dentro de
F70 sin encabezado separado; el número quedó saltado en la numeración
secuencial, no hay contenido perdido.

**F56** (tabla de particiones a medida) es infraestructura de toda la
app, no del componente — ver
[`app_arquitectura.md`](app_arquitectura.md).

### F57 — `AccessKeyFetcher` falla siempre al pedir el token: era `CONFIG_CSPOT_CLIENT_SECRET` vacío, no una regresión de TLS (Medio, ✅ confirmado en hardware — no era un bug de código)

Logs reales de hardware (después de F55/F56) muestran `Certificate
validated` (el handshake TLS contra `accounts.spotify.com` sí completa)
seguido, cada ~4 segundos indefinidamente, de `AccessKeyFetcher.cpp:129:
Failed to fetch access token` — sin ningún log de "Failed to parse..."
en el medio. La sesión de Spotify en sí sigue andando (Mercury, SPIRC,
`Got audio key`, nombre/duración de pista), así que en un primer momento
parecía plausible que fuera un efecto secundario de haber activado la
verificación de certificados (F55) — pero el análisis del código y una
prueba directa contra el endpoint real apuntan a otra causa.

**Por qué no parece ser un problema de TLS/red**: si `esp_crt_bundle_attach()`
fallara la validación, `TLSSocket::open()` (línea 69-74) tiraría una
excepción en el propio handshake, antes de llegar siquiera a leer una
respuesta — y el log de arriba confirma que el handshake sí completa.
Tampoco aparece el log `"Failed to parse access token response"` (rama
`nlohmann`, no compilada acá) ni ningún crash - y la rama que sí compila
(`BELL_ONLY_CJSON`) solo llega silenciosamente a "Failed to fetch access
token" en dos casos: `cJSON_Parse()` devolvió null (respuesta vacía/no
JSON) o el JSON síparseó pero no tenía `access_token`/`expires_in`, o
tenía una clave `"error"`.

**Prueba directa** (`curl --http1.1` contra
`https://accounts.spotify.com/api/token` con credenciales inventadas,
para no arriesgar las reales del usuario): Spotify responde
`HTTP/1.1 400 Bad Request`, con `Content-Length` (no chunked, así que no
es un problema de que este cliente HTTP minimalista no soporte
`Transfer-Encoding: chunked` — se consideró y se descartó como hipótesis)
y cuerpo `{"error":"invalid_client","error_description":"Invalid
client"}`. Ese cuerpo, si el dispositivo lo recibiera, produce
exactamente el síntoma observado: JSON válido, sin excepción, pero con
clave `"error"` presente → cae directo a "Failed to fetch access token"
sin loguear nada más específico. Es la explicación que mejor encaja con
los logs, sin necesitar ninguna interacción con TLS/certificados.

**Causas más probables, a revisar del lado de la cuenta/app de Spotify**
(no de este código): `CONFIG_CSPOT_CLIENT_ID`/`CONFIG_CSPOT_CLIENT_SECRET`
incorrectos o desactualizados, la app registrada en
developer.spotify.com fue revocada/eliminada, o esa app no tiene
habilitado el flow "Client Credentials" (algunas apps nuevas del
dashboard de Spotify lo requieren activar aparte).

**Diagnóstico agregado temporalmente** (`AccessKeyFetcher.cpp`): en la
rama de fallo (rama `BELL_ONLY_CJSON`, la que compila este proyecto), se
agregó logging de los campos `error`/`error_description` del cuerpo de
la respuesta, para no tener que asumir la causa. **Confirmado por el
usuario antes incluso de reflashear con ese logging**: faltaba
configurar `CONFIG_CSPOT_CLIENT_SECRET` (`idf.py menuconfig` →
`CSpot (Spotify Connect) component`) — exactamente la causa "credenciales"
que se había identificado como más probable, no un bug de este código ni
mucho menos de F55/F56. Con el secret configurado, se espera que
`AccessKeyFetcher` funcione normalmente.

El logging de diagnóstico se **revirtió** (`AccessKeyFetcher.cpp` quedó
igual que antes de este hallazgo) una vez confirmada la causa — ya
cumplió su propósito y no hace falta dejarlo permanentemente. Compila
limpio, mismo margen de flash (~22%).

### F58 — Silencio total al reanudar tras una pausa larga: conexión al CDN muerta y sin timeout de lectura (Alto, encontrado en hardware real, ✅ corregido y confirmado en hardware)

Reportado con logs reales: pausar y esperar varios minutos (en el caso
reportado, ~7m47s) antes de reanudar hace que la reproducción no emita
ningún sonido — sin logs de error, sin `EOF`, sin crash. Los logs muestran
la sesión de Spotify funcionando con normalidad (`External play command`,
`playback: play`, paquetes de Mercury llegando) — el problema es
puramente de audio.

**Causa raíz, en tres partes que se encadenan**:

1. `CDNAudioFile` (`CDNAudioFile.cpp`) abre **una sola conexión HTTP/TLS
  persistente** al arrancar la pista (`this->httpConnection`) y la
  **reutiliza** para cada rango de bytes siguiente durante toda la
  reproducción (`readBytes()` → `this->httpConnection->get(...)`, misma
  conexión, sin reabrir).
2. Nuestra implementación de pausa (F52, en `cspot_connect.cpp` — no en
  este código vendorizado) hace que `TrackPlayer` reintente el mismo
  chunk de PCM ya decodificado sin tocar la red mientras está pausado.
  Confirmado en `TrackPlayer.cpp:220-249`: `VORBIS_READ()` (que es lo
  único que dispara una lectura de red, vía `_vorbisRead()` →
  `CDNAudioFile::readBytes()`) sólo se llama de nuevo una vez que el
  chunk actual fue *completamente* entregado — con `pcmWrite()` devolviendo
  0 todo el tiempo que dura la pausa, esa condición nunca se cumple. Es
  decir: durante una pausa larga, la conexión al CDN queda completamente
  inactiva, sin ningún tráfico, todo el tiempo que dure la pausa.
3. Pasado cierto tiempo sin tráfico (típicamente unos pocos minutos), un
  NAT/firewall intermedio (muy común en redes celulares o domésticas) da
  de baja esa conexión de su tabla de traducción **sin avisarle a
  ninguno de los dos lados** (no manda ni FIN ni RST) — por eso no hay
  ningún error visible. Y **`TLSSocket.cpp` no tenía ningún timeout de
  lectura configurado**: al reanudar, `readBytes()` reutiliza esa
  conexión zombie, pide un nuevo rango, y la lectura de la respuesta
  (`mbedtls_ssl_read()` dentro de `TLSSocket::read()`) queda **bloqueada
  para siempre** esperando una respuesta que nunca va a llegar — de ahí
  el silencio total sin ningún log.

**Un problema adicional, descubierto al diseñar la corrección**: agregar
sólo un timeout de lectura no alcanza. `SocketBuffer::underflow()`
(`SocketStream.cpp`) ya señaliza `eof()` correctamente cuando
`TLSSocket::read()` devuelve 0 (comportamiento preexistente, ver F30) —
pero `HTTPClient::Response::readResponseHeaders()` (`HTTPClient.cpp`)
nunca revisaba el estado del stream (`socketStream.fail()`) dentro de su
`while(1)`, sólo el resultado de `phr_parse_response()` y si el buffer se
llenó. Con un stream ya fallido, cada `getline()` devuelve 0 bytes
**instantáneamente, sin bloquear** — sin el chequeo agregado, el timeout
de lectura convertía un cuelgue silencioso en un **busy-spin infinito**
(sin `sleep`, sin ceder CPU), que es peor, no mejor.

**Corrección aplicada, en tres archivos** (todos vendorizados de
`cspot`/`bell`, no código propio de este componente):

1. `TLSSocket.cpp`: `mbedtls_ssl_conf_read_timeout(&conf, 15000)` +
  `mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, NULL,
  mbedtls_net_recv_timeout)` (con `f_recv` en `NULL` a propósito —
  mbedTLS usa `f_recv_timeout` en vez de `f_recv` en cuanto está seteado,
  confirmado en `library/ssl_msg.c`). Una lectura que no recibe nada en
  15s falla con `MBEDTLS_ERR_SSL_TIMEOUT` en vez de bloquear para
  siempre.
2. `HTTPClient.cpp`, `readResponseHeaders()`: se agregó
  `if (socketStream.fail()) throw std::runtime_error(...)` justo después
  de cada `getline()` — cierra el hueco del busy-spin para cualquier
  causa de fallo de lectura (timeout, cierre, lo que sea), no sólo la de
  este hallazgo puntual.
3. `CDNAudioFile.cpp`, `readBytes()`: la petición sobre la conexión
  existente ahora está en un `try/catch`; si falla, se reintenta **una
  vez** abriendo una conexión nueva (`bell::HTTPClient::get()`) — la URL
  firmada del CDN sigue siendo válida durante varios minutos, así que una
  conexión nueva debería funcionar sin problema. Si el reintento también
  falla, se loguea el error y se devuelve `0` (interpretado como EOF por
  vorbis) en vez de dejar escapar la excepción — importante porque esta
  función se llama desde `TrackPlayer::_vorbisRead()`, invocada por
  libvorbisfile a través de un puntero a función C
  (`ov_open_callbacks`), que no es seguro de desenrollar con una
  excepción de C++ sin terminar el programa.

Compila limpio, mismo margen de flash (~22%, la lógica agregada no pesa
lo suficiente para notarse). **Confirmado en hardware real**: se repitió
el escenario (pausar varios minutos, reanudar) y el audio se escucha
correctamente.

**Mejora de seguimiento, mismo hallazgo**: el usuario notó que el
mecanismo de arriba, siendo correcto, deja un corte perceptible de hasta
~15s al reanudar (el tiempo que tarda en dispararse el timeout de
`TLSSocket` antes de recién ahí reconectar) — es **reactivo**: espera a
que la lectura falle para recién entonces reconectar. Se cambió a un
enfoque **proactivo** en `CDNAudioFile.cpp`/`.h`:

- Se agregó `lastRequestTime` (`std::chrono::steady_clock::time_point`,
  sin dependencia de plataforma) a `CDNAudioFile`, actualizado en cada
  petición exitosa al CDN (tanto en `openStream()` como en `readBytes()`).
- En `readBytes()`, antes de reutilizar `this->httpConnection`, se
  calcula cuánto pasó desde la última petición exitosa. Si superó
  `STALE_CONNECTION_THRESHOLD` (20s — elegido para quedar cómodamente por
  debajo de los tiempos de expiración de NAT/carrier típicos, sin ser tan
  corto como para gatillar en pausas cortas normales), se reconecta
  **directamente**, sin intentar primero la conexión vieja ni esperar su
  timeout. Abrir una conexión nueva cuesta unos cientos de ms (handshake
  TLS), no los ~15s del camino reactivo.
- El `try/catch` con reintento en la conexión existente sigue como
  respaldo para el caso más raro de una conexión que muere en pleno uso
  pese a actividad reciente (ahí el chequeo de tiempo ocioso no aplica, y
  sigue haciendo falta el timeout de `TLSSocket` como red de seguridad).

Con esto, una pausa corta (segundos) sigue reutilizando la conexión sin
ningún costo extra; una pausa larga reconecta proactivamente en cuanto se
reanuda, sin el corte de ~15s. Compila limpio, mismo margen de flash
(~22%). **Confirmado en hardware real por el usuario** que el fix de base
funciona; la mejora proactiva (reducir el corte perceptible) quedó
aplicada y compilando limpio, pendiente de una confirmación específica
en hardware de que el corte efectivamente se redujo.

### F59 — La conexión de sesión (Mercury/AP) ya tiene keepalive y reconexión automática propios — aclaración, no un bug (Informativo, ✅ confirmado funcionando en hardware real)

Surgió al comparar con `librespot` (que implementa un intercambio
Ping/Pong/PongAck explícito para detectar una sesión caída): investigando
si a `cspot` le faltaba ese mecanismo, resultó que **ya lo tiene**,
completo y funcionando — mecanismo distinto del que se corrigió en F58,
y sobre una conexión distinta.

**Lo que ya existe, verificado leyendo el código** (todo vendorizado, no
agregado en esta sesión):

- Spotify manda un paquete `Ping` (comando `4`) por la conexión
  Shannon/AP cada ~2 minutos. `MercurySession::runTask()`
  (`MercurySession.cpp:60-64`) lo detecta y responde automáticamente con
  un `Pong` (`shanConn->sendPacket(0x49, packet.data)`) — es exactamente
  el patrón ping/pong que usa `librespot`, sólo que ya estaba
  implementado acá también (heredado de `cspot` upstream, no algo que
  haya faltado portar).
- `PlainConnection.cpp` configura `SO_RCVTIMEO`/`SO_SNDTIMEO` en el
  socket (líneas 95-101) — sin eso, el mecanismo de abajo sería código
  muerto (el socket bloquearía para siempre, igual que le pasaba a
  `TLSSocket` antes de F58). Con el timeout de socket configurado, cada
  vez que un `recv()`/`send()` da `EAGAIN`/`ETIMEDOUT`
  (`readBlock()`/`writeBlock()`, líneas 176-230) se consulta
  `timeoutHandler()` (`Session::triggerTimeout()` →
  `MercurySession::triggerTimeout()`), que compara cuánto hace del
  último `Ping` recibido contra `PING_TIMEOUT_MS` (2min5s — mismo
  esquema de "margen de seguridad sobre el intervalo esperado" que usa
  `librespot` con sus 60s+5s). Si no se superó ese umbral, simplemente
  reintenta la lectura/escritura; si se superó, tira `"Reconnection
  required"`.
- Esa excepción la atrapa `MercurySession::runTask()` (línea 68), que
  llama a `reconnect()` — un bucle que reintenta `connectWithRandomAp()`
  + `authenticate(this->authBlob)` cada 5s hasta lograrlo (bucle
  reescrito en F32 para no crecer el stack en cada intento). Es
  **reconexión completamente automática, sin reemparejamiento ZeroConf**
  — reutiliza el `authBlob` ya guardado de la sesión original.

**Por qué esto no contradice lo dicho en la sección 5 (F17) sobre "no
reintenta la sesión automáticamente sin repetir el emparejamiento
ZeroConf"**: esa nota es sobre un escenario más angosto y distinto —
la conexión **inicial** (`Session::connectWithRandomAp()` +
`authenticate()`, antes de que `MercurySession::runTask()` siquiera
arranque su bucle) fallando por completo, ej. durante el emparejamiento
mismo. El mecanismo de F59 sólo entra en juego **una vez que la sesión ya
está establecida y corriendo** — ahí sí reconecta solo, sin intervención
del usuario ni de `cspot_connect.cpp`. Son dos capas distintas: una vez
adentro, la sesión se cuida sola; el gap real y todavía vigente es sólo
si la conexión inicial nunca llega a establecerse.

**Por qué el CDN (F58) sí necesitó una solución construida a mano**: la
conexión al CDN es HTTP genérico contra un host de Spotify que no es
parte del protocolo propio (`audio-ak.spotifycdn.com`, sin `Ping`/`Pong`
de aplicación disponible) — no hay una señal del servidor equivalente
para reutilizar, por eso ahí hubo que resolverlo con timeout de socket +
reconexión reactiva/proactiva en vez de responder a un keepalive nativo
como el de Mercury.

Ningún cambio de código en este hallazgo — es puramente una aclaración
de arquitectura, documentada para que quede constancia de que este
mecanismo existe y no hace falta reimplementarlo.

**Confirmado en hardware real, en uso normal (no una prueba forzada)**:
en una sesión larga de varias horas, la conexión al AP se cortó dos veces
por una razón de red genuina (`Error while receiving packet: Error in
read`, no timeout por falta de ping — el corte pasó justo después de un
ping recibido normalmente), y las dos veces `MercurySession::reconnect()`
recuperó la sesión sola (`Reconnection successful`) sin intervención del
usuario ni reemparejamiento ZeroConf. De paso, en la misma sesión también
se confirmó F58 funcionando: tras una pausa larga, `CDNAudioFile` logueó
`"CDN connection idle for a while, reconnecting proactively..."` y la
reproducción retomó sin problema.

**Nota aparte, no relacionada con este hallazgo**: en esa misma prueba
hubo un corte que en un primer momento pareció una desconexión de
Spotify, pero el log mostró `rst:0x1 (POWERON), boot:0x20
(DOWNLOAD(USB/UART0))` — un reset por corte de alimentación/USB real
(la laptop durmiéndose o el cable USB reconectándose resetea la placa vía
DTR/RTS del adaptador serie), no un crash de firmware ni un problema de
sesión. Para pruebas largas sin el monitor serie activo todo el tiempo,
conviene alimentar la placa de una fuente aparte en vez de depender del
puerto USB del monitor.

### F60 — Soporte de audio MP3 para episodios de podcast (Media/Alto, aplicado — pendiente de confirmar en hardware)

Investigando por qué los episodios de podcast no se podían reproducir
(reportado por el usuario) se encontró que `cspot` sí tiene soporte de
protocolo/metadata para episodios (`TrackReference` distingue
`track:`/`episode:`, `TrackQueue::loadPbEpisode()` parsea toda la
metadata), pero **la selección de archivo de audio sólo pedía formatos
`OGG_VORBIS_*`** (`TrackQueue.cpp::stepParseMetadata()`,
`ctx->config.audioFormat`) — y los podcasts de Spotify suelen estar
codificados en MP3, no Vorbis. Aunque se pidiera el formato correcto,
`TrackPlayer.cpp` sólo sabía decodificar Vorbis (`libvorbisfile`/tremor
directo, sin ninguna abstracción de códec plegable).

**Hallazgo colateral durante el diseño**: `bell` ya trae vendorizado un
decoder MP3 completo (`libhelix-mp3`, apagado por
`BELL_CODEC_MP3 OFF`) con dos wrappers distintos y **no relacionados
entre sí** — ninguno usado por `cspot` hasta este cambio:
- `bell::MP3Decoder` (`bell/main/audio-codec/MP3Decoder.cpp`) — wrapper
  delgado y correcto, reserva su buffer de salida con el tamaño real que
  necesita un frame (`MAX_NSAMP*MAX_NGRAN*MAX_NCHAN*sizeof(int16_t)` =
  4608 bytes).
- `bell::EncodedAudioStream` (`bell/main/io/EncodedAudioStream.cpp`) —
  **tiene un bug de desborde de buffer real, nunca ejercitado porque
  nada lo llama**: su `outputBuffer` es `std::vector<short>(2*2*4*4)` =
  **128 bytes**, muy por debajo de los 4608 que un frame real de MP3
  puede necesitar — `MP3Decode()` escribiría bien más allá del buffer en
  el primer frame real. También tiene una resta sin chequear en su
  camino de resync (`bytesInBuffer -= 3800` sin verificar que haya esa
  cantidad disponible). No se tocó ni se usó este archivo — sigue
  exactamente igual que antes, código muerto.

**Diseño elegido, confirmado con el usuario**: usar el `libhelix-mp3` ya
vendorizado en `bell` (no el paquete `esp-libhelix-mp3` del ESP Component
Registry — sería una segunda copia del mismo decoder), a través de
`bell::MP3Decoder` (el wrapper correctamente dimensionado), con un loop
de lectura/resync **nuevo y acotado** escrito directamente en
`TrackPlayer.cpp` — mismo nivel de integración directa que ya tiene hoy
el camino Vorbis (sin pasar por las abstracciones genéricas de `bell`) —
en vez de arreglar y reusar `EncodedAudioStream`.

**Corrección aplicada**:
1. `components/CMakeLists.txt (repo root)`: `BELL_CODEC_MP3` `OFF` → `ON`. Se
  eliminó el workaround de F1b (`target_include_directories` manual para
  que `mp3dec.h` resolviera con el codec apagado) — ya redundante, ese
  mismo path ahora lo agrega `bell/CMakeLists.txt` de verdad.
2. `TrackQueue.h`/`TrackQueue.cpp`: `QueuedTrack` gana un campo
  `selectedFormat` (se perdía antes — sólo se guardaba el `fileId`
  elegido, no el formato). `stepParseMetadata()` gana un tercer nivel de
  fallback: si no matcheó ni el formato configurado ni el fallback a
  Vorbis 96kbps, prueba cualquier `MP3_*` disponible (prefiriendo
  160kbps) — es lo que realmente permite cargar episodios que no tengan
  ninguna variante Vorbis, el caso típico.
3. `TrackPlayer.h`/`TrackPlayer.cpp`: `runTask()` ahora bifurca según
  `track->selectedFormat` — Vorbis usa el camino existente sin ningún
  cambio de lógica; MP3 usa un nuevo método privado `_mp3DecodeFrame()`
  (busca sync word con `MP3FindSyncWord()`, decodifica con
  `bell::MP3Decoder`, con reintento de resync acotado a 8 intentos en
  vez del descuento fijo sin chequear que tiene el código de referencia)
  que replica exactamente el contrato de retorno de `VORBIS_READ`
  (`>0` = bytes de PCM, `0` = EOF, `<0` = error irrecuperable), para que
  el resto del loop (entrega a `dataCallback`, manejo de pausa/reset) sea
  idéntico entre los dos formatos.

**Limitación conocida, no resuelta acá**: no hay soporte de seek para
MP3 — un pedido de seek en un episodio se loguea y se descarta,
arrancando siempre desde el principio. Vorbis tiene seek real
(`ov_time_seek`, usa las tablas del contenedor Ogg); MP3 no tiene ese
mecanismo — implementarlo necesitaría estimar posición por bitrate o
escanear frames, fuera de alcance de este cambio.

**Sample rate/canales**: se mantiene el `audioSink->setParams(44100, 2,
16)` hardcodeado que ya se usaba para Vorbis, también para MP3, en vez
de conectar reconfiguración dinámica por pista. Si en hardware aparece
distorsión de tono/velocidad en algún episodio (formato real distinto a
44.1kHz/estéreo), es la señal para agregar esa reconfiguración como
mejora de seguimiento.

**Costo de flash medido**: habilitar `BELL_CODEC_MP3` (mayormente las
tablas Huffman/trigonométricas de `libhelix-mp3`, datos `const` en
`.rodata`) más el código nuevo bajó el margen libre de **~22% a ~20%**
(~46.5 KB). Compila limpio, sin necesitar el workaround `-Wno-error` que
sí hizo falta para `tremor` (F1c) — `libhelix-mp3` no dispara los mismos
warnings bajo GCC 15.

Compila limpio (`idf.py build`, 0 errores). **Confirmado en hardware
real por el usuario**: episodios de podcast reproducen — en las pruebas
hechas, vía el fallback a `OGG_VORBIS_96` (formato `0`, ver más abajo),
no se confirmó todavía el camino de decodificación MP3 en sí en
hardware.

**Hallazgo de seguimiento: `metadata.proto` traía el enum `AudioFormat`
desactualizado más allá del índice 7** — corregido. Probando episodios
reales aparecieron valores de formato fuera de rango de nuestro enum
(`File format: 12`, `File format: 10`), que `nanopb` decodificó bien
igual (los enums de protobuf son enteros crudos; un valor sin nombre
conocido no rompe nada, sólo no se puede nombrar en el código) pero que
nuestra lógica de selección no podía reconocer ni matchear. Comparado
contra el `.proto` real de `librespot` (verificado con dos fuentes
independientes: el `.proto` fuente en `librespot-golang` y la
documentación generada de `librespot-metadata`, ambas coinciden) se
encontró que nuestros valores `8`/`9` estaban mal nombrados
(`AAC_24`/`AAC_48`, deberían ser `MP4_128_DUAL`/`OTHER3`) y faltaban
`10=AAC_160`, `11=AAC_320`, `12=MP4_128`, `13=OTHER5` por completo. Se
corrigió `protobuf/metadata.proto` para que coincida — nada más cambió
(ningún código de este proyecto referenciaba los nombres viejos
`AAC_24`/`AAC_48`, verificado por grep). Compila limpio, mismo margen de
flash (los `.proto` se compilan a código en el momento del build, vía
`nanopb_generate_cpp`, no hay artefactos generados versionados que
actualizar a mano).

**Implicación real, sin resolver todavía**: los formatos nuevos que
aparecieron en los logs (`MP4_128`, `AAC_160`, `MP4_128_DUAL`) son AAC
empaquetado en contenedor MP4 — ni Vorbis ni el MP3 nuevo de este
hallazgo los cubren. En las dos pruebas reales, ambos episodios también
ofrecían `OGG_VORBIS_96` (formato `0`) en su lista de archivos, así que
cayeron ahí por el fallback existente y reprodujeron bien — pero un
episodio cuya lista de formatos fuera *sólo* AAC/MP4 (sin ninguna
variante Vorbis ni MP3) seguiría fallando con "File not available for
playback", no por estar genuinamente restringido sino por no tener
ningún formato que este proyecto sepa decodificar. Agregar soporte
AAC/MP4 real es un trabajo bastante más grande que este (necesita además
desempaquetar el contenedor MP4, no sólo activar el decoder AAC de
`bell`, que asume stream ADTS crudo) — evaluado como posible trabajo de
seguimiento, no aplicado en este hallazgo.

**Nota aparte, no relacionada con el formato de audio**: en las pruebas
también se observó el mismo patrón de "Track failed to load, skipping
it" repitiéndose en loop, ya visto y documentado en F58/la investigación
de F59 — se sigue viendo con algunos episodios (uno de los casos
confirmados como genuinamente restringido por Spotify, `restriction.size()
= 1` sin ningún archivo disponible). Este patrón (el cliente reintentando
`Load` en loop mientras el dispositivo no logra confirmar reproducción a
tiempo) parece preexistente, independiente del soporte de MP3, y no se
investigó a fondo en este hallazgo.

**F61** (rework de flash/particiones a 16MB) es infraestructura de toda
la app, motivada por sumar la UI con display — ver
[`app_arquitectura.md`](app_arquitectura.md).

### F62 — `cspot_connect.h`/`.cpp` gana control local de reproducción: `play/pause/next/previous()` (Alto, aplicado, ✅ confirmado en hardware)

Hasta acá, `cspot_connect.h` era puramente informativo (solo reacciona a
comandos remotos vía SPIRC, nunca los origina) — necesario para la
implementación de UI con display que arrancó en esta etapa (pantallas de
WiFi setup y player, ver
[`app_arquitectura.md`](app_arquitectura.md), F62 en ese documento).

Se agregaron `cspot_connect_play/pause/next/previous()` en
`cspot_connect.h`/`.cpp`, wrappeando `cspot::SpircHandler::setPause()`/
`nextSong()`/`previousSong()` (ya existentes en el motor vendorizado,
`SpircHandler.h`, nunca antes expuestos fuera de `cspot_connect.cpp`).
`setPause()` internamente hace `sendEvent(EventType::PLAY_PAUSE, ...)`, que
reentra sincrónicamente en `CSpotConnectPlayer::eventHandler()` — o sea que
un play/pause local dispara el mismo camino (`notify()` → el callback de
la app) que ya usa un play/pause remoto, sin duplicar lógica de estado.

El problema real era de *threading*: `spirc` (el `SpircHandler`) solo se
tocaba antes desde la propia tarea de sesión (`runSessionInner()`'s
`ctx->session->handlePacket()` → `handleFrame()` → ... → métodos de
`spirc`). Los botones locales de la UI corren en la tarea de LVGL/touch —
una tercera tarea que ahora también toca `spirc`. Se agregó un
`std::mutex spircMutex` en `CSpotConnectPlayer`, tomado tanto alrededor de
`handlePacket()` como en los cuatro métodos nuevos
(`requestPlayPause/Next/Previous`), así que sesión remota y control local
nunca tocan `spirc` en simultáneo. **Deliberadamente no** se protegió con
el mismo mutex a `notifyAudioReachedPlayback()`/`notifyAudioEnded()`
(llamadas desde `pcmWrite()`, que corre en la tarea propia de
`TrackPlayer` — una carrera ya preexistente, no introducida acá) para no
arriesgar un deadlock por reentrancia sin visibilidad completa del código
interno de `TrackPlayer`/`TrackQueue` — alcance deliberadamente acotado a
los cuatro puntos de entrada nuevos.

**Riesgo conocido, confirmado más tarde en hardware**: el mutex envuelve
la llamada completa a `handlePacket()`, no solo el acceso a `spirc` — si
esa llamada bloquea esperando el próximo paquete de red, un caller en
otra tarea podría tardar en responder hasta que llegue el siguiente
paquete. Este riesgo se materializó como un congelamiento total de la UI
— ver F70 en `app_arquitectura.md`.

**Verificado**: `idf.py build` limpio, 0 errores. **Confirmado en
hardware**: el control local (play/pause/next/previous) efectivamente
cambia la reproducción cuando se invoca — la cadena completa de
estabilización que hizo falta para llegar a eso está documentada en
`app_arquitectura.md` (F63-F74, algunas de las cuales también están acá
mismo — ver la tabla en la sección 9).

### F63 — Reinicio del dispositivo al refrescar el access token: `bell::HTTPClient::post()` sin try/catch en `AccessKeyFetcher` (Alto, encontrado en hardware real, corregido — pendiente confirmar en hardware)

Primer bug real encontrado probando la implementación de F62 en hardware:
el dispositivo completaba el pairing, autenticaba, sincronizaba reloj con
los servidores de Spotify, y se suscribía a Mercury — pero apenas
`TrackQueue` intentaba refrescar el access token (`AccessKeyFetcher.cpp`,
mensaje "Access token expired, fetching new one...") el dispositivo se
reiniciaba con un `abort()`/`std::terminate()`.

**Causa**: `bell::TLSSocket::open()` (`external/bell/main/io/TLSSocket.cpp:87`)
hace `throw std::runtime_error("mbedtls_ssl_handshake error")` si el
handshake TLS falla (acá, `config returned -135`, un fallo transitorio de
conexión a `accounts.spotify.com`). Ese throw se propaga sin que nada lo
atrape a través de `SocketBuffer::open()` → `SocketStream::open()` →
`HTTPClient::Response::connect()` → `HTTPClient::post()` →
`AccessKeyFetcher::updateAccessKey()` → `getAccessKey()` →
`TrackQueue::runTask()` — que corre en la tarea propia de `TrackQueue`
("CSpotTrackQueue"), **no** la tarea de sesión que envuelve `runSession()`
con try/catch (F17). Sin ningún try/catch en ese camino, la excepción
llega sin atrapar hasta el entry point de la tarea → `std::terminate()` →
`abort()` → reinicio completo del dispositivo.

Esto es un hueco que dejó **F26** (que sí agregó try/catch alrededor del
*parseo* de la respuesta JSON, en el mismo archivo/función) sin cubrir: el
try/catch de F26 empieza *después* de la llamada a `bell::HTTPClient::
post(...)` — la llamada de red en sí, la parte que de hecho lanza acá,
quedaba completamente desprotegida.

**Corrección aplicada** (`AccessKeyFetcher.cpp`, `updateAccessKey()`):
la llamada a `bell::HTTPClient::post(...)` ahora está en su propio
try/catch — un fallo de red/TLS se trata igual que cualquier otro intento
fallido de conseguir el token (log + `BELL_SLEEP_MS(3000)` + reintento,
hasta 3 veces), en vez de escaparse sin atrapar.

Nota aparte: el log también mostró un primer intento de conexión de sesión
(no de `AccessKeyFetcher`) que falló con
`esp-x509-crt-bundle: PSA signature verification failed` /
`mbedtls_ssl_handshake error` — pero ese sí lo atrapa el try/catch de F17
en `cspot_connect.cpp`'s `runSession()`, solo lo logueó
("session failed...") y el dispositivo se recuperó solo en el siguiente
intento (autenticó bien ~33s después). No requirió corrección.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~51% libre). **Pendiente de confirmar en hardware real** que el fix
efectivamente evita el reinicio (probar hasta que se dispare un refresh
de token real, o forzar el escenario si es reproducible).

**Actualización — segunda prueba en hardware real**: F63 cumplió lo que
prometía (ya no hay reinicio), pero reveló el problema de fondo: el
refresh del access token falla **siempre**, en loop indefinido cada 3s,
nunca se recupera. Ver **F64**.

### F64 — `AccessKeyFetcher` nunca logra refrescar el token: buffers de mbedTLS sin lugar en DRAM interna (Alto, encontrado en hardware real, corregido — pendiente confirmar en hardware)

Con F63 aplicado (ya no hay `abort()`), el log mostró el fallo real: cada
intento de `AccessKeyFetcher::updateAccessKey()` fallaba con
`TLSSocket.cpp:87: failed! config returned -135` — el mismo código que ya
había aparecido una vez en la sesión anterior (`config returned -135`), y
emparentado con el `0xffffff73 (decimal: -141)` de la primera prueba
(F63). **-135 y -141 no son códigos de mbedTLS clásicos — son códigos de
la capa PSA crypto** (`PSA_ERROR_INVALID_ARGUMENT` y
`PSA_ERROR_INSUFFICIENT_MEMORY` respectivamente, mbedTLS 4.0 corre todo el
RNG/crypto sobre PSA — ver `psa_init.h`). La intuición del usuario
("¿quizás memoria?") apuntaba en la dirección correcta.

**Causa, confirmada por Kconfig (no solo por sospecha)**:
`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` (el default de ESP-IDF) fuerza que
**todos** los buffers de mbedTLS salgan de DRAM interna, nunca de PSRAM.
`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384` + `_OUT_CONTENT_LEN=4096`
(`CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` apagado, o sea tamaño fijo,
no negociado) — cada conexión TLS nueva necesita ~20KB contiguos de DRAM
interna solo para esos buffers, sin contar el resto del contexto SSL. El
log de boot (`heap_init`) mostraba apenas ~259KB de DRAM interna libre
*antes* de que arranquen Wi-Fi y el LVGL/display de F62 — ambos
consumidores pesados de esa misma DRAM interna (los buffers de Wi-Fi no
pueden vivir en PSRAM; el framebuffer de LVGL si vive en PSRAM gracias a
F61, pero widgets/estructuras internas de LVGL no). Para cuando
`AccessKeyFetcher` (en la tarea de `TrackQueue`) intenta abrir una
conexión TLS **nueva** hacia `accounts.spotify.com` — aparte de la que ya
está viva para la sesión — probablemente no queda un bloque contiguo de
~20KB libre en DRAM interna, y la capa PSA lo reporta como error de
argumento inválido o memoria insuficiente según en qué paso interno
falle la alocación.

**Corrección aplicada**: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` en vez de
`INTERNAL_MEM_ALLOC` (`sdkconfig.defaults` + `sdkconfig` — mismo patrón de
edición manual que F61, porque el archivo ya tenía la clave vieja
explícita). Mueve esos buffers a PSRAM (8MB, con mucho margen libre —
ver F61/F62). El propio texto de ayuda del Kconfig de ESP-IDF recomienda
exactamente esta alternativa para aplicaciones con DRAM interna
limitada; la advertencia de "interno es más seguro" que menciona es sobre
modelos de amenaza con flash encryption activo, no aplicable acá (no se
usa flash encryption).

**Diagnóstico agregado** (`TLSSocket.cpp`, junto al `throw` de
`mbedtls_ssl_handshake`): un log de `heap_caps_get_free_size`/
`heap_caps_get_largest_free_block` sobre `MALLOC_CAP_INTERNAL` en el
momento exacto del fallo — temporal, para confirmar con números reales en
la próxima prueba si esto efectivamente resuelve el problema o si hace
falta seguir investigando (dejarlo si se quiere seguir monitoreando esa
métrica, o quitarlo una vez confirmado).

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~51% libre — el partition sizing de F61 no se ve afectado, esto es
memoria RAM en runtime, no flash). **Pendiente de confirmar en hardware
real**: que `AccessKeyFetcher` efectivamente consiga el token esta vez, y
revisar los números de heap interno logueados si todavía falla.

**Actualización — tercera prueba en hardware real**: el error PSA
(-135/-141) durante el *handshake* TLS ya no aparece — el fix de F64
sirvió para eso. Pero `AccessKeyFetcher` ahora falla un paso antes, en la
resolución DNS (`mbedtls_net_connect failed`, código -82 =
`MBEDTLS_ERR_NET_UNKNOWN_HOST`), en loop indefinido, junto con warnings
`wifi:m f null` del propio driver de WiFi (un malloc interno fallando) -
señal de que la presión de memoria interna sigue ahí, solo que ahora se
nota en otro consumidor. El diagnóstico de heap de F64 solo estaba
enganchado en el fallo del *handshake*, no en el del *connect* - extendido
acá (ver más abajo) para cubrir ambos puntos. Además apareció un **crash
nuevo y distinto**, no relacionado con `AccessKeyFetcher` - ver **F65**.

### F65 — Segundo crash sin relación con AccessKeyFetcher: `handlePost()` (pairing ZeroConf) sin try/catch (Alto, encontrado en hardware real, corregido — pendiente confirmar en hardware)

Mientras la sesión seguía trabada reintentando `AccessKeyFetcher` (F64),
llegó un **segundo POST de ZeroConf pairing** (el teléfono reintentando
seleccionar el dispositivo, probablemente porque desde su punto de vista
la primera sesión no terminó de "prender" — nunca llegó a reproducir
audio real). Ese segundo pairing hizo `abort()` con un backtrace
totalmente distinto al de F63/F64:

```
CryptoMbedTLS::pbkdf2HmacSha1(...) at Crypto.cpp:287
cspot::LoginBlob::decodeBlobSecondary(...) at LoginBlob.cpp:111
cspot::LoginBlob::loadZeroconf(...) at LoginBlob.cpp:136
cspot::LoginBlob::loadZeroconfQuery(...) at LoginBlob.cpp:227
CSpotConnectPlayer::handlePost(httpd_req*) at cspot_connect.cpp:231
... (esp_http_server internals, tarea httpd)
```

**Causa**: `pbkdf2HmacSha1()` (`Crypto.cpp:287`) hace `throw std::
runtime_error("pbkdf2HmacSha1 failed")` si **cualquier** paso de la
derivación de clave vía PSA (`psa_key_derivation_setup/input_integer/
input_bytes/output_bytes`) no devuelve `PSA_SUCCESS` — la misma capa PSA
que ya venía fallando por presión de memoria en F63/F64, esta vez durante
una operación de derivación de clave en lugar de una conexión TLS.
`handlePost()` (`cspot_connect.cpp:231`, el handler HTTP del pairing
ZeroConf) llama a `blob->loadZeroconfQuery(queryMap)` **sin ningún
try/catch** — a diferencia de `runSession()` (F17, tarea de sesión) y
ahora `AccessKeyFetcher::updateAccessKey()` (F63, tarea de `TrackQueue`),
este código corre en una **tercera tarea** (la del servidor HTTP,
`httpd`), sin ninguna protección. Es el mismo patrón de hueco que F17/F63
ya habían corregido en otros dos lugares, solo que en un tercero que nunca
se había ejercitado con una segunda solicitud de pairing concurrente.

**Corrección aplicada** (`cspot_connect.cpp`, `handlePost()`): la llamada
a `blob->loadZeroconfQuery(queryMap)` ahora está en su propio try/catch —
si falla, se loguea y se responde `500 Internal Server Error` al teléfono
(que reintentará el pairing) en vez de reiniciar todo el dispositivo.

**Diagnóstico extendido** (`TLSSocket.cpp`): el log de heap interno de
F64 se factorizó a una función `logInternalHeap()` y ahora se llama tanto
en el fallo de `mbedtls_net_connect()` (el que se vio en esta prueba)
como en el de `mbedtls_ssl_handshake()` (el de la prueba anterior) — antes
solo cubría el segundo.

**Nota**: la causa de fondo (presión de memoria interna que se sigue
manifestando en distintos lugares - TLS, PSA key derivation, buffers de
WiFi) probablemente no está 100% resuelta todavía solo con F64. Este
hallazgo cierra el crash puntual de `handlePost()`, pero no investiga por
qué la DNS/conexión de `AccessKeyFetcher` sigue sin lograr conectar - los
próximos logs (con el diagnóstico ahora en ambos puntos) deberían decir
si todavía es DRAM interna agotada o si es otra cosa (ej. el propio
resolver DNS de lwIP, o contención real con la ráfaga de reintentos cada
3s de F64 saturando algo).

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~51% libre). **Pendiente de confirmar en hardware real**: que un segundo
pairing concurrente ya no reinicie el dispositivo, y los números de heap
interno logueados en el próximo fallo de `AccessKeyFetcher` (si persiste).

**Actualización — cuarta prueba en hardware real, con los números de heap
ya en mano**: ambos fixes (F64, F65) funcionaron según lo esperado -
**cero reinicios** en toda la prueba. Secuencia observada: `AccessKeyFetcher`
falló dos veces (`internal heap: 5823 free, 3328 largest` en el primer
fallo — DNS —, cayendo a `591 free, 352 largest` en el segundo — handshake
—) y **logró el token al tercer intento**. En el medio, la sesión
principal (`MercurySession`) también perdió y recuperó la conexión sola
("Error while receiving packet" → "Closing socket..." → reconectó), hubo
un fallo puntual de `mdns_send` ("Cannot allocate memory"), y un segundo
pairing ZeroConf falló limpio con `500` en vez de crashear (F65
confirmado en hardware).

**Se revisó la cadena de ownership de `TLSSocket` para descartar una fuga
real** (no solo contención pasajera) como explicación de por qué el heap
cae tanto entre el primer y segundo intento: `SocketBuffer::open()`
(`SocketStream.cpp:16`) asigna `internalSocket` (`std::make_unique<TLSSocket>()`)
**antes** de llamar a `internalSocket->open(...)` (la llamada que tira la
excepción) — y `TLSSocket`'s constructor ya deja `isClosed = false`, así
que si `open()` tira a mitad de camino, `HTTPClient::Response`'s
destructor (que corre correctamente vía el `std::unique_ptr<Response>`
local de `HTTPClient::post()`, incluso con la excepción en vuelo) sí ve
`isOpen() == true` y llama `close()`, liberando `mbedtls_net_free`/
`mbedtls_ssl_free`/`mbedtls_ssl_config_free` como corresponde. **La cadena
RAII está bien** — no hay fuga de recursos de mbedTLS por excepción sin
atrapar en ningún punto de este camino.

**Conclusión de esta ronda**: la caída de heap entre los dos intentos de
`AccessKeyFetcher` coincide con una ventana de reconexión concurrente
real (WiFi + mDNS + `MercurySession` reconectando + el propio
`AccessKeyFetcher` reintentando, todo en simultáneo) — contención de pico
transitoria, no un leak permanente y monótono; el sistema se recupera
solo apenas esa ventana pasa (de hecho el token se consiguió inmediatamente
después del punto más bajo). Con F63/F64/F65 aplicados, el comportamiento
ante esta contención pasó de "reinicio duro" a "reintento con eventual
éxito", que es un piso razonable para seguir probando el resto de la UI
(player screen, cover art, controles) en vez de seguir exprimiendo
margen de DRAM interna sin una señal concreta de que siga haciendo falta.

**F66** (presión de DRAM interna sostenida: WiFi/lwIP + umbral de
`malloc()` a PSRAM) es una decisión de Kconfig de sistema completo, no
del componente cspot — ver [`app_arquitectura.md`](app_arquitectura.md).
Confirmado en hardware que funcionó (heap interno recuperado, tracks
reales cargando por primera vez), pero reveló un problema nuevo — ver
F67 a continuación.

### F67 — Contención del AES por hardware bajo carga concurrente + pool interno fijo de LVGL (Alto, aplicado, ✅ confirmado en hardware)

**Causa #1 (AES)**: revisando `esp_aes_dma_core.c` (ESP-IDF), los
descriptores DMA que necesita el acelerador de AES por hardware del
ESP32-S3 están forzados a `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` — a
diferencia de los buffers propios de mbedTLS (F64), **no existe
alternativa en PSRAM para descriptores DMA en este SoC**, es una
restricción física. El ESP32-S3 tiene un solo periférico AES compartido;
la ráfaga de fallos casi simultáneos coincide con una ventana de
actividad criptográfica concurrente real: streaming del track actual +
prefetch del siguiente (audio key + URL de CDN, propio de `TrackQueue`) +
el fetch de cover art de F62, todos haciendo TLS/AES en la misma ventana
de ~1s.

**Causa #2 (LVGL)**: LVGL usa por default su propio allocador interno
(`LV_USE_BUILTIN_MALLOC`) con un pool **fijo de 64KB**
(`CONFIG_LV_MEM_SIZE_KILOBYTES`), completamente al margen del sistema
`heap_caps`/`malloc()` de ESP-IDF — ninguno de los ajustes de F64/F66
(`SPIRAM_MALLOC_ALWAYSINTERNAL`, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) le
aplicaba. Cada widget/estilo/buffer de dibujo de LVGL quedaba anclado a
DRAM interna sin posibilidad de ir a PSRAM - un costo fijo de 64KB sobre
un presupuesto de ~259KB al boot.

**Corrección aplicada** (`sdkconfig.defaults` + `sdkconfig`, mismo patrón
manual ya usado en F61/F64/F66):
- `CONFIG_LV_USE_CLIB_MALLOC=y` (en vez de `LV_USE_BUILTIN_MALLOC`): LVGL
  pasa a usar `malloc()`/`free()` estándar, que sí respeta las
  preferencias de PSRAM ya configuradas.
- `CONFIG_MBEDTLS_HARDWARE_AES=n` (con `GCM_SUPPORT_NON_AES_CIPHER` y
  `AES_USE_INTERRUPT` apagados también, dependientes de esa opción):
  elimina por completo la necesidad de descriptores DMA internos para
  AES, cayendo a AES por software. La documentación de Espressif para
  `MBEDTLS_HARDWARE_AES` es explícita al respecto (confirmado por el
  usuario, 2026-07-20): *"Note that if the ESP32 CPU is running at
  240MHz, hardware AES does not offer any speed boost over software
  AES."* Este proyecto corre a 240MHz desde F61/F62, así que aplica
  directo - este cambio elimina la fuente de contención sin costo de
  rendimiento medible.
  Costo de *seguridad* (no de performance): el AES por software resuelto
  acá usa T-tables (`CONFIG_MBEDTLS_AES_ROM_TABLES=y`), teóricamente más
  expuesto a ataques de canal lateral por timing de caché que el
  acelerador por hardware - riesgo práctico bajo para este dispositivo
  (sin atacante co-residente posible). Ver `docs/aprendizaje.md`,
  entrada "Seguridad: TLS, verificación de certificados y mbedTLS",
  sección 6, para el detalle completo.

**✅ Confirmado en hardware real**: ni un solo error de memoria/AES en la
séptima prueba. Sesión estable, varios tracks encolados con audio key y
URL de CDN, y **`TrackPlayer.cpp:216: Playing` — el audio arrancó a
reproducirse por primera vez en todo este trabajo**. Binario incluso un
poco más chico (`0x1ee6f0` vs `0x1f1ac0` antes, 52% libre vs 51% - se
descartó el código del driver de AES por hardware). La cadena de
estabilización de la UI que siguió (stack overflow, freeze de botones,
posición de reproducción, prioridades de FreeRTOS) está documentada en
[`app_arquitectura.md`](app_arquitectura.md), F68 en adelante.

**F68** (stack overflow en `player_ui_task`) y **F70** (los botones
congelaban la UI por `spircMutex` mal repartido entre tareas) son ambas
código de `components/ui/` — ver
[`app_arquitectura.md`](app_arquitectura.md). F70 en particular es la
confirmación en hardware del riesgo ya anotado arriba en F62.

### F71 — Posición de reproducción real en vez de estimación por reloj local (Medio, aplicado, ✅ confirmado en hardware)

El diseño original de la barra de progreso (F62) estimaba el tiempo
transcurrido contando desde un timestamp local (`esp_timer_get_time()`)
capturado en cada `CSPOT_EVENT_PLAY`. Funciona mientras el audio fluye sin
cortes, pero se desincroniza en dos casos reales: si la red se traba y el
audio deja de alimentarse (la barra sigue avanzando igual, porque cuenta
tiempo de pared, no audio real), y ante cualquier *seek* remoto (desde el
celular u otro dispositivo Spotify Connect) - `cspot_connect.h` no tenía
forma de enterarse de eso.

**Encontrado revisando el motor vendorizado** (no algo pedido
explícitamente, surgió al buscar cómo resolver esto mejor):
`SpircHandler` ya usa internamente una `PlaybackState` con
`innerFrame.state.position_ms` + `position_measured_at`
(`PlaybackState.h`/`.cpp`) para poder reportarle su posición a los
servidores de Spotify - actualizada en cada carga de track, cada
play/pause **local o remoto**, y cada seek remoto
(`SpircHandler.cpp:162,202`: `playbackState->updatePositionMs(playbackState
->remoteFrame.position)` ante un frame remoto). Es decir, ya existe una
fuente de verdad completa para la posición, más precisa que cualquier
cosa que se pudiera reconstruir desde afuera - solo no estaba expuesta.

**Cambios aplicados**:
- `SpircHandler.h`/`.cpp` (vendorizado, mismo criterio que F62 para
  play/pause/next/previous - exponer algo que el motor ya calcula
  internamente): nuevo `uint32_t getPositionMs()`, que extrapola
  `position_ms + (ahora - position_measured_at)` solo si el estado es
  `Play` (si está pausado, devuelve el valor ya congelado) - la misma
  cuenta que `PlaybackState::setPlaybackState(Paused)` ya hace
  internamente al pausar, expuesta como getter en vez de quedar interna.
- `cspot_connect.h`/`.cpp`: nuevo `cspot_connect_get_position_ms()`,
  mismo patrón mutex-protegido que `cspot_connect_play/pause/next/
  previous()` (F62).
- `player_screen.cpp` consume este getter vía `progress_poll_task` — ver
  [`app_arquitectura.md`](app_arquitectura.md), F71 (parte de la app).

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). **Confirmado en hardware real**: la posición de reproducción
queda sincronizada con la fuente de verdad de SPIRC, incluyendo tras un
*seek* remoto. El ajuste de scheduling de las tareas de UI que hizo falta
para que esto funcionara de forma fiable (F72-F74) es código de
`components/ui/` — ver [`app_arquitectura.md`](app_arquitectura.md).

## 10. Auditoría de código adicional (F75-F100, julio 2026): excepciones sin capturar, condiciones de carrera, y hardening informado por forks/proyectos hermanos

**F101 y F102 (la suite de tests de host, `components/tests/`)
se movieron a [`docs/host_tests.md`](host_tests.md)** — siguen siendo
parte de la misma secuencia global de F-numbers, solo cambiaron de
documento por alcance (igual que ya pasa entre este documento y
`app_arquitectura.md`).

Auditoría dirigida, buscando específicamente dos clases de problema que ya
se habían visto antes en este proyecto pero nunca se habían revisado de
forma sistemática en todo el árbol de tareas: (a) el mismo patrón de F26/
F63/F65 (una excepción sin capturar que se escapa del entry point de una
tarea de FreeRTOS y tira el dispositivo entero), y (b) estado compartido
entre tareas sin sincronización, en el estilo del riesgo que F62 ya había
señalado como conocido.

### F75 — `TrackPlayer::runTask()` no captura excepciones, pero `CDNAudioFile::openStream()` sí puede lanzar (Alto, encontrado por auditoría, corregido)

De las cuatro tareas de FreeRTOS del proyecto (`TrackPlayer`,
`MercurySession`, `TrackQueue`, `CSpotConnectPlayer`), `TrackPlayer::
runTask()` (`TrackPlayer.cpp`) era la única sin un solo `try/catch` en
toda la función — pese a llamar a `CDNAudioFile::openStream()`
(`CDNAudioFile.cpp:43-86`), que puede lanzar de tres formas: un `throw
std::runtime_error(...)` explícito si el CDN responde con un header más
corto de lo esperado (línea 61), y dos operaciones de red sin protección
(`bell::HTTPClient::get()` y `.stream().read()`) que también pueden
lanzar. A diferencia de `readBytes()` (misma clase), que ya quedó bien
protegido con try/catch alrededor de sus reintentos de red tras F58,
`openStream()` nunca se revisó — y se ejecuta **una vez por cada track**,
justo el tipo de operación de red en vivo donde F58 ya demostró que hay
reconexiones/cortes reales. Sin captura, cualquiera de esas tres fallas
se escapa de `runTask()`, del entry point C de `bell::Task`, y aborta el
dispositivo entero — el mismo bug de fondo que F26 (parseo JSON en
`TrackQueue`), F63 (`AccessKeyFetcher::post()`) y F65 (`handlePost()` de
ZeroConf) ya corrigieron en otras tareas, pero nunca se había auditado
para esta.

**Corrección aplicada** (`TrackPlayer.cpp`): la llamada a `openStream()`
ahora está en su propio `try/catch`. Al fallar, se loguea el error, se
limpia `currentTrackStream` (queda a medio inicializar si `openStream()`
lanzó a mitad de camino), y se llama a `this->eofCallback()` seguido de
`continue` — el mismo camino de recuperación que la función ya usa unas
líneas antes para "track failed to load, skipping it" (línea ~165), así
que no es un patrón nuevo: un track con un problema de red al abrir se
saltea y el player sigue con el siguiente, en vez de reiniciar el
dispositivo entero.

**Deliberadamente fuera de alcance**: los loops de decodificación (Vorbis/
MP3) que siguen a `openStream()` llaman a `readBytes()` indirectamente
(vía los callbacks de `libvorbis` o `_mp3DecodeFrame()`), pero
`readBytes()` ya captura sus propias excepciones de red internamente
(F58) y devuelve `0` en vez de lanzar — no se encontró una ruta real de
excepción sin capturar ahí, así que no se tocó nada más para mantener el
fix acotado a lo verificado.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real que un fallo real
de red al abrir un track (difícil de reproducir a demanda) efectivamente
salta al siguiente track en vez de reiniciar el dispositivo.

### F76 — `lastTrackId` sin sincronización entre `pcmWrite()` y `eventHandler()`: condición de carrera real (Medio, encontrado por auditoría, corregido)

`cspot_connect.cpp`: `lastTrackId` (un `std::string` normal, no atómico)
se leía y escribía sin ningún lock desde `pcmWrite()` — que corre en la
tarea propia de `TrackPlayer`, no la de `CSpotConnectPlayer` — mientras
que `eventHandler()` lo escribía (`.clear()`) en el caso
`PLAYBACK_START`. Desde F62, `eventHandler()` puede correr tanto en la
tarea de sesión (vía `handlePacket()`, dentro de `spircMutex`) como en la
tarea de control local — botones — (vía `requestNext()`/
`requestPrevious()`, también dentro de `spircMutex`). El problema es que
`pcmWrite()` nunca toma `spircMutex`, así que esa protección no cubre en
absoluto la interacción entre las dos tareas: dos hilos distintos podían
tocar el mismo `std::string` al mismo tiempo — comportamiento indefinido,
no solo un valor desactualizado (una lectura a mitad de una reasignación
interna de `std::string` puede leer un puntero/tamaño inconsistentes).

**Por qué no se corrigió tomando `spircMutex` en `pcmWrite()`**: sería la
solución obvia, pero `pcmWrite()` corre en el hot path de audio (se llama
por cada chunk de PCM entregado, continuamente durante la reproducción).
F70 ya dejó documentado que `spircMutex` envuelve la llamada *completa* a
`handlePacket()`, que puede bloquear esperando el próximo paquete de red
— tomar ese mismo mutex en `pcmWrite()` arriesgaría exactamente el tipo
de congelamiento que F70 corrigió para la UI, pero esta vez en el propio
pipeline de audio. Se descartó por sentido común, no se llegó a probar en
hardware.

**Corrección aplicada**: en vez de sincronizar el acceso a `lastTrackId`,
se eliminó la necesidad de que otro hilo lo toque. Nuevo
`std::atomic<bool> forceTrackRestart{false}` — `eventHandler()` en
`PLAYBACK_START` ahora hace `forceTrackRestart = true` en vez de tocar
`lastTrackId` directamente. `pcmWrite()` chequea `lastTrackId != trackId
|| forceTrackRestart.exchange(false)` para decidir si hay que
re-notificar `notifyAudioReachedPlayback()` — con esto, `lastTrackId`
(el `std::string`) queda confinado por completo a un solo hilo (el de
`TrackPlayer`, vía `pcmWrite()`), y la señal cruzada entre tareas pasa a
ser un `std::atomic<bool>` sin lock, seguro para acceso concurrente. El
comportamiento observable es idéntico al original: un `PLAYBACK_START`
sigue forzando una re-notificación aunque el track ID no haya cambiado
(por ejemplo, Spotify reiniciando el mismo track desde el principio).

**Nota**: el `lastTrackId.clear()` en `runSessionInner()` (antes de crear
`spirc` y arrancar la sesión) no forma parte de esta carrera — ocurre
antes de que `pcmWrite()` pueda ser invocado siquiera (todavía no hay
callback registrado), así que se dejó sin cambios.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real que el
comportamiento de "reached playback" sigue disparándose correctamente en
cada cambio de track y en un reinicio del mismo track.

### F77 — `AudioSink::flush()`: silencio inmediato al pausar, en vez de esperar a que drene el buffer (Bajo, aplicado)

Retoma la limitación conocida documentada en F52: pausar detenía
correctamente el avance de la pista, pero lo que ya estaba en el ring
buffer de 32KB (`BufferedAudioSink::dataBuffer`) más los descriptores DMA
de I2S (`dma_desc_num=10 * dma_frame_num=1000`, F20/F51) seguía sonando
hasta vaciarse — hasta ~600ms en el peor caso con ambos buffers llenos
(mono; ~300ms en estéreo), no perceptible como bug funcional pero sí como
demora real al pausar.

**Diseño**: nuevo `virtual void flush() {}` en `AudioSink` (no-op por
defecto — no rompe otros sinks), sobreescrito en `BufferedAudioSink`.
`CSpotConnectPlayer::eventHandler()` lo llama en el caso `PLAY_PAUSE`
cuando `isPaused == true`, junto al `paused` atómico ya existente.

**Por qué `flush()` no vacía el ring buffer directamente**: los ring
buffers de FreeRTOS no soportan más de un receptor concurrente de forma
segura, y `i2sFeedTask()` (la tarea dedicada del sink) ya es quien hace
`xRingbufferReceiveUpTo()` en su propio loop — si `flush()` (llamada
desde el hilo de sesión/control, vía `eventHandler()`) intentara vaciar
el buffer directamente, sería un segundo receptor concurrente sobre el
mismo ring buffer. En cambio, `flush()` solo prende un
`std::atomic<bool> flushRequested`; `i2sFeedTask()` lo chequea al
principio de cada vuelta de su propio loop y, si está prendido, drena y
descarta todo lo que haya en el ring buffer (con `xRingbufferReceiveUpTo`
sin bloqueo) y además deshabilita/rehabilita el canal I2S — el mismo par
`i2s_channel_disable()`/`i2s_channel_enable()` que `setParams()` ya usa
para reconfiguración en caliente, reutilizado acá para descartar lo que
ya estaba entregado a los descriptores DMA.

**Cambio de latencia acotado, no cosmético**: el timeout de
`xRingbufferReceiveUpTo()` en el loop normal bajó de `portMAX_DELAY` a
100ms — si no hubiera nada nuevo para reproducir, antes la tarea quedaba
bloqueada indefinidamente sin forma de notar un flush pendiente. Con
`portMAX_DELAY`, un flush pedido durante un tramo sin audio nuevo
tendría que esperar al próximo chunk real para poder chequear la bandera.
100ms de latencia máxima extra en el caso normal (con audio fluyendo, el
buffer casi nunca está vacío, así que este timeout casi nunca se cumple)
a cambio de que el flush sea confiable incluso en silencio.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real que el corte al
pausar es efectivamente inmediato (sin el remanente de cientos de ms de
F52) y que no introduce ningún click/pop audible por el
disable/enable del canal I2S.

### F78 — `TrackQueue::preloadedTracks` mutado sin `tracksMutex` desde `processTrack()`: condición de carrera real (Alto, encontrado en hardware real, corregido)

Reportado por el usuario en hardware real: sesión difícil de conectar y,
más notablemente, **reproduciendo un track distinto al que mostraba el
cliente de Spotify**. El log mostró un patrón sospechoso y reproducible:
el mismo track ID se "conseguía" y abría dos veces seguidas en
`TrackPlayer.cpp`, sin ningún `Resetting state` entre medio:

```
Got track ID=5e9410d5...
Opening HTTP stream to .../5e9410d5...
Header and footer bytes received
Got track ID=5e9410d5...          <- el mismo ID otra vez
Opening HTTP stream to .../5e9410d5...  <- reabre el stream
```

**Causa**: `TrackQueue::consumeTrack()` (usado por `TrackPlayer` para
pedir "el track después de este") busca el track anterior dentro de
`preloadedTracks` con `std::find()`; si no lo encuentra, un fallback lo
manda de vuelta a `preloadedTracks[0]` en vez de fallar — comportamiento
sospechoso en sí mismo, pero la causa raíz está un nivel más abajo:
`TrackQueue::runTask()` (línea ~450) copia `preloadedTracks` a una
variable local bajo `tracksMutex`, **libera el lock**, y recién ahí itera
la copia llamando a `processTrack()` por cada track — es decir,
`processTrack()` corre **fuera** de `tracksMutex`. Su caso
`CDN_REQUIRED`, al terminar de cargar un track, llama a
`queueNextTrack()`, que hace `push_back()`/`push_front()`/`pop_front()`
sobre el `preloadedTracks` real — **sin ningún lock**. Mientras tanto,
`TrackPlayer` (`consumeTrack()`), `SpircHandler` (`skipTrack()`,
`updateTracks()`, ambos disparados por comandos remotos) sí toman
`tracksMutex` correctamente antes de tocar el mismo deque. El resultado
es una mutación concurrente sin sincronizar de un `std::deque` desde dos
tareas distintas (la propia de `TrackQueue` vs. la de `TrackPlayer`/
sesión) — comportamiento indefinido, consistente con que `consumeTrack()`
lea el deque a mitad de una modificación y no encuentre el track
anterior, cayendo en el fallback que repite/pierde tracks.

**Nota al margen**: `getTrackInfo()` (`TrackQueue.cpp:423-429`) también
itera `preloadedTracks` sin lock, mismo tipo de gap — pero no tiene
ningún caller en todo el árbol (código muerto hoy), así que se dejó sin
tocar.

**Corrección aplicada** (`TrackQueue.cpp`, caso `CDN_REQUIRED` de
`processTrack()`): se agregó `std::scoped_lock lock(tracksMutex)`
alrededor de la llamada a `queueNextTrack()` — acotado a eso únicamente,
no a toda la función, porque `stepLoadCDNUrl()` (la línea anterior, en el
mismo case) hace una request HTTP bloqueante de verdad (segundos, según
el propio log del usuario); tomar el mutex durante esa espera hubiera
bloqueado a `TrackPlayer`/`SpircHandler` por ese mismo tiempo cada vez
que se precarga un track — el mismo tipo de riesgo que F70 ya enseñó a
evitar.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real que el track
reproducido coincide de forma consistente con el que muestra el cliente
de Spotify.

### F79 — Bug propio (F76): evaluación de cortocircuito deja `forceTrackRestart` sin consumir, dispara un avance de cola espurio (Alto, encontrado en hardware real con logging de diagnóstico, corregido y confirmado en hardware)

Reportado por el usuario: el cliente de Spotify y el display avanzan de
track (a veces dos veces) sin que el audio cambie — se sigue escuchando
el track anterior. Con el logging de diagnóstico agregado para investigar
F78, el log de hardware mostró la causa exacta:

```
pcmWrite: trackId change  -> 831b3558...        <- 1ra llamada, track genuinamente nuevo
notifyAudioReachedPlayback: notifyPending=1     <- consume el pending, no saltea (correcto)

pcmWrite: trackId change 831b3558 -> 831b3558   <- 2da llamada, MISMO id (solo el siguiente chunk de PCM)
notifyAudioReachedPlayback: notifyPending=0
skipTrack: NEXT, currentTracksIndex 10 -> 11    <- avanza la cola igual
skipped to head=f1184516...                     <- UI muestra este, audio sigue en 831b3558
```

**Causa**: bug propio, introducido en F76. La condición en `pcmWrite()`
era:

```cpp
if (lastTrackId != trackId || forceTrackRestart.exchange(false)) {
```

En la primera llamada de un track nuevo, `lastTrackId != trackId` ya es
`true` (compara contra el `lastTrackId` vacío o el del track anterior) —
y en C++ el operador `||` con el lado izquierdo `true` **no evalúa el
lado derecho**. `forceTrackRestart.exchange(false)` nunca se ejecuta esa
vez, así que si la bandera estaba prendida (la deja prendida
`eventHandler()` en cada `PLAYBACK_START`, ver F76), **queda sin
consumir**. En la siguiente llamada de `pcmWrite()` (mismo `trackId` —
un chunk de PCM normal, no un track nuevo), el lado izquierdo ya es
`false`, así que ahora sí se evalúa el lado derecho — y encuentra la
bandera todavía prendida de la vez anterior, disparando
`notifyAudioReachedPlayback()` una segunda vez para un track que no
cambió. Esa segunda llamada no tiene ningún `notifyPending` que
consumir, así que cae en la rama que hace `skipTrack(NEXT)` de verdad —
avanzando el índice de la cola (lo que ve el cliente/display) sin que
`TrackPlayer` haya cambiado de track en absoluto.

**Corrección aplicada** (`cspot_connect.cpp`, `pcmWrite()`): se separó la
evaluación en dos variables antes del `if`, para que
`forceTrackRestart.exchange(false)` se ejecute siempre, sin depender de
cortocircuito:

```cpp
bool trackChanged = lastTrackId != trackId;
bool forcedRestart = forceTrackRestart.exchange(false);
if (trackChanged || forcedRestart) { ... }
```

**Nota**: esto no descarta el problema de threading real que sí
corrigió F78 (`preloadedTracks` mutado sin `tracksMutex`) — son dos bugs
independientes, ambos capaces de producir un desincronismo entre la cola
y lo que realmente suena. F79 es, con evidencia directa de hardware, la
causa confirmada del síntoma reportado por el usuario en esta ronda;
F78 sigue siendo una corrección válida por sí misma.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). **✅ Confirmado en hardware real** por el usuario. El
logging de diagnóstico temporal (`pcmWrite`, `notifyAudioReachedPlayback`,
`skipTrack`) se retiró una vez confirmado — cumplió su propósito de
capturar la secuencia exacta en un log real en vez de seguir adivinando
por lectura de código.

### F80 — `stepLoadCDNUrl()` parseaba la respuesta de storage-resolve sin chequear status HTTP ni validez del JSON (Medio, aplicado)

Encontrado evaluando el fork [CastaliaInstitute/cspot](https://github.com/CastaliaInstitute/cspot/tree/codex/astrolabe-185-spotify-cspot)
(rama `codex/astrolabe-185-spotify-cspot`, comparada contra
`feelfreelinux/cspot` upstream) — su commit "Add modern Spotify storage
resolve support" hace bastante más que un hardening (migra a un endpoint
distinto, `spclient.wg.spotify.com`, con parseo de protobuf incluido), y
eso no se adoptó — pero la parte de manejo de errores sí es un problema
real que también existe en nuestro `stepLoadCDNUrl()`, sobre el endpoint
actual (`api.spotify.com/v1/storage-resolve`, sin cambios).

`QueuedTrack::stepLoadCDNUrl()` (`TrackQueue.cpp`) tomaba el body de la
respuesta HTTP y lo pasaba directo al parser JSON, sin chequear el
status code ni si el body vino vacío — un rate-limit, un backend caído,
o cualquier respuesta no-2xx terminaban intentando parsearse igual, con
un mensaje de error genérico (`"Cannot fetch CDN URL"`, sin detalle de la
causa). Además, en la rama `BELL_ONLY_CJSON` (la que compila en este
proyecto), `cJSON_Parse()`/`cJSON_GetObjectItem()`/`cJSON_GetArrayItem()`
pueden devolver `null` en un fallo de parseo, y el código original los
desreferenciaba sin chequear — un null deref (crash) latente ante una
respuesta malformada o inesperada, nunca disparado en las pruebas hechas
hasta ahora pero real.

**Corrección aplicada** (`bell/main/io/include/HTTPClient.h`/`.cpp`,
`TrackQueue.cpp`):
- `bell::HTTPClient::Response` gana un `int statusCode() const` (portado
  del fork de `bell` referenciado más arriba — el nuestro nunca exponía
  el status code que `readResponseHeaders()` ya parseaba internamente,
  solo lo usaba para decidir cuándo cortar el loop de parseo).
- `stepLoadCDNUrl()`: chequea `statusCode()` fuera de 200-299 y body
  vacío antes de intentar parsear, logueando status + los primeros 240
  bytes del body en caso de error - visibilidad real en vez de
  `"Cannot fetch CDN URL"` a secas.
- Chequeo de `null` después de cada paso del parseo cJSON, con mensaje
  específico de qué falló.
- `catch (...)` reemplazado por `catch (const std::exception&)` con
  `e.what()` + un `catch (...)` de respaldo — mismo patrón que F63/F65.

**Deliberadamente no adoptado** del fork: el cambio de endpoint a
`spclient.wg.spotify.com` (API interna/no documentada de los clientes
oficiales, más propensa a cambios sin aviso) y su parser de protobuf —
nuestro endpoint actual sigue funcionando (confirmado en todos los logs
de esta sesión), así que no había necesidad de migrar solo para quedarse
con el manejo de errores.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real que el logging
nuevo aparece correctamente ante un fallo real de storage-resolve (no
reproducido a demanda, solo verificado que compila y no cambia el
comportamiento del camino exitoso).

**Plan de contingencia, documentado pero no implementado**: si
`api.spotify.com/v1/storage-resolve` llegara a romperse (algo que
Spotify ya demostró que hace con endpoints legacy — ver
[librespot-org/librespot#1623](https://github.com/librespot-org/librespot/issues/1623)/
[#1622](https://github.com/librespot-org/librespot/pull/1622), aunque
ese caso puntual fue sobre el endpoint de *metadata* legacy
`/metadata/4/...` de librespot vía HTTP, no sobre storage-resolve — `cspot`
ni siquiera usa ese endpoint de metadata, obtiene la metadata vía Mercury
(`hm://metadata/3/...`, `TrackQueue.cpp:383`), así que ese incidente
concreto no aplica directo acá), la alternativa evaluada es migrar a
`https://spclient.wg.spotify.com/storage-resolve/files/audio/interactive/{fileId}?product=0&country={country}&salt={salt}`
(el endpoint que usa el fork de CastaliaInistitute, tomado de lo que usan
los clientes oficiales), con las siguientes salvedades a resolver **antes**
de adoptarlo, no descubrirlas en el camino:
- Puede requerir un *client token* (`clienttoken.spotify.com`), distinto
  del access token OAuth que ya obtiene `AccessKeyFetcher` — no
  confirmado si hace falta para este endpoint puntual.
- La respuesta puede venir en protobuf en vez de JSON (`content-type:
  application/x-protobuf`) — necesitaría el parser de varints que trae
  el fork (`firstStorageResolveCdnUrl()` en su `TrackQueue.cpp`), o
  forzar JSON si el endpoint lo permite vía query param.
- Depende de `ctx->config.countryCode` ya poblado en el momento de la
  llamada — verificar que el orden de inicialización lo garantice.

Gracias a F80, si este endpoint actual empieza a fallar de verdad, ahora
va a quedar visible en el log (status HTTP + body) en vez de fallar en
silencio — esa sería la señal para revisar esta migración con evidencia
real en vez de preventivamente.

### F81 — Riesgo arquitectónico a futuro: `librespot` reemplazó Mercury por "Dealer" (WebSocket) para SPIRC/Connect (Informativo, no accionable hoy)

Investigando [`librespot`](https://github.com/librespot-org/librespot) (la
implementación de referencia en Rust, más completa/activa que `cspot`) en
busca de hallazgos de protocolo/protobuf que valga la pena tener
documentados. No encontré nada de protobuf directamente portable —
`librespot` tiene 90+ `.proto` propios, pero la enorme mayoría son
esquemas de telemetría/analytics (`AdEvent.proto`, `CacheReport.proto`,
etc.) que `cspot` no necesita, y ya usa la librería correcta para este
hardware (`nanopb`, liviana, vs. el `prost` de Rust de `librespot`). Pero
apareció algo más importante mirando el changelog:

> v0.7.0 (agosto 2025): "Replaced Mercury usage in Spirc with Dealer"

**Qué es Dealer**: confirmado revisando el código
(`core/src/dealer/manager.rs`) — es un transporte por **WebSocket**
(`wss://{host}:{port}/?access_token={token}`), reemplazando a Mercury (el
protocolo binario por TCP que usa `cspot` hoy, con URIs `hm://...`) para
todo lo relacionado a SPIRC/estado de Connect. Las URIs de más alto nivel
siguen pareciéndose (`hm://connect-state/v1/cluster`), pero ya no viajan
por Mercury.

**Por qué es relevante para este proyecto**: `cspot`/`MercurySession`/
`SpircHandler` están construidos enteramente sobre Mercury — es el único
transporte que tenemos para SPIRC, suscripciones, y todo lo visto en
esta sesión. Si Spotify llegara a deprecar Mercury para SPIRC (como ya
hizo con el endpoint legacy de metadata de `librespot`, ver
[#1623](https://github.com/librespot-org/librespot/issues/1623)/
[#1622](https://github.com/librespot-org/librespot/pull/1622), aunque ese
caso puntual fue un endpoint distinto), no sería un fix puntual como los
de esta sesión (F63/F65/F78/F79/F80) — sería un rework arquitectónico:
implementar un cliente WebSocket + el protocolo/framing de Dealer, y
probablemente reescribir buena parte de `SpircHandler`.

**Lo que sí reduce el riesgo, si algún día hiciera falta**: la
autenticación de Dealer usa el mismo tipo de pieza que ya tenemos
vendorizada — host/puerto se resuelven vía `apresolver().resolve("dealer")`
(mismo mecanismo de `ApResolve.cpp` que ya usamos, pidiendo el servicio
`"dealer"` en vez de `"accesspoint"`), y el token es
`session.login5().auth_token().access_token` — Login5, el mismo flujo que
ya está vendorizado acá (`protobuf/login5.proto`) y, con alta probabilidad,
el mismo tipo de token que ya maneja `AccessKeyFetcher`. O sea: no haría
falta resolver autenticación desde cero, "solo" el transporte WebSocket y
el framing de mensajes de Dealer sobre ese socket.

**No accionable hoy**: Mercury nos sigue funcionando sin problemas en
todos los logs de esta sesión. Esto queda como nota de riesgo a vigilar,
no como tarea pendiente.

### F82 — `HTTPClient::rawRequest()` no reconectaba una conexión keep-alive muerta; `memcpy` sin guardar en `readResponseHeaders()` (Medio, aplicado)

Encontrado evaluando el fork de `bell` de
[philippe44](https://github.com/philippe44/bell/compare/43730fbc8840e46ce0091d04384d1cde6c89c756...ed2d6e9cad2206196dbfdd2df8e46f5453df66bf)
(mantiene el `bell` que usa `squeezelite-esp32`, la fuente original de
este vendorizado — más cercano a "nuestro" código que un fork lejano).
Dos fixes en `main/io/HTTPClient.cpp`; se adoptaron los dos que aportaban
algo real, se descartó el tercero por redundante.

**Adoptado 1 — reconexión automática en `rawRequest()`**: hasta ahora,
si una conexión keep-alive reutilizada moría entre llamadas (el servidor
la cierra del otro lado, típico de una sesión ociosa), `rawRequest()` no
lo detectaba antes de escribir el siguiente request — el único lugar de
todo el proyecto con protección para esto era `CDNAudioFile::readBytes()`
(F58), a mano, con su propio try/catch alrededor de cada llamada. Todo
lo demás que usa `bell::HTTPClient` (`stepLoadCDNUrl()`/storage-resolve,
recién endurecido en F80, `AccessKeyFetcher`, `ApResolve`) no tenía
ninguna protección equivalente. Se generalizó el patrón de F58 dentro de
`rawRequest()` mismo: si el socket no está abierto o no está en buen
estado, reconecta antes de escribir; si `readResponseHeaders()` falla
después de escribir, reintenta **una vez** — pero solo si el problema fue
la conexión en sí (`!socketStream.good()`), nunca ante un error de
protocolo genuino (respuesta malformada, demasiado grande), que se
relanza de inmediato sin reintentar.

**Adoptado 2 — guarda en el `memcpy` de restauración de delimitadores**
(`readResponseHeaders()`): la resta `httpBufferAvailable - 2` es sobre
`size_t` (sin signo) — si `httpBufferAvailable` fuera menor a 2 en la
primera vuelta del loop (una respuesta degenerada/corrupta), desborda a
un valor enorme y el `memcpy` escribe fuera de rango. No disparado nunca
en este proyecto, pero real y barato de corregir.

**Descartado — chequeo alternativo de conexión cerrada en
`readResponseHeaders()`** (`eof()`/`bad()`/`gcount()==0` +
`fail()` separado para "Response too large"): cubre el mismo bug que ya
tenemos resuelto acá desde antes (comentario existente citando F58,
usando `socketStream.fail()` + el chequeo de `httpBufferAvailable ==
httpBuffer.size()` para "too large") — mismo efecto por un camino
distinto, sin ningún problema nuevo que justifique reescribirlo.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real que una conexión
keep-alive muerta a mitad de sesión (el escenario que F58 ya documentó
como real, "CDN connection idle for a while") se recupera igual de bien
para `stepLoadCDNUrl()`/`AccessKeyFetcher` como ya lo hace para
`CDNAudioFile`.

### F83 — Cortes de audio periódicos: el margen entre buffer y latencia de red del CDN era demasiado ajustado (Alto, medido en hardware real, corregido)

Reportado por el usuario: cortes de audio recurrentes, sin patrón obvio
("por momentos pasan varios segundos sin fallos, pero luego vuelve a
suceder"), sin ningún error visible en el log.

**Diagnóstico**: antes de tocar nada, se calculó el margen teórico entre
la cadencia de pedidos al CDN y el colchón de buffering disponible.
`CDNAudioFile::HTTP_BUFFER_SIZE` (14KB de audio comprimido por
range-request) a 160kbps equivale a ~0.72s de audio por fetch. El
colchón para absorber la latencia de ese fetch era el ring buffer de
`BufferedAudioSink` (32KB de PCM **mono** — confirmado
`CONFIG_CSPOT_I2S_MONO_OUTPUT=y` — a 88,200 bytes/s, ~0.37s) más el
margen del DMA de I2S — un colchón total de ~300-400ms contra fetches
que ocurren cada ~0.7s. Un margen ya ajustado de por sí en el diseño
original (ninguno de los buffers involucrados se había tocado en esta
sesión antes de este finding).

**Confirmado con datos reales**: se agregó logging temporal midiendo
cada round-trip de CDN (`CDNAudioFile.cpp`, ver instrumentación abajo).
En ~2.5 minutos de reproducción real, la mayoría de los fetches fueron
sanos (80-250ms), pero con picos recurrentes muy por encima del colchón
disponible: 574ms, 548ms, 816ms, 1055ms, 1131ms, 1357ms, 1495ms, 1547ms,
1810ms, y un pico de **3528ms** — todos con `reused=1` (la conexión
keep-alive nunca necesitó reconectar; es latencia de red/CDN real, no un
problema de manejo de conexión). Los picos aparecían cada 5-20 fetches
(~3-15s de reproducción), coincidiendo exactamente con el patrón
reportado.

**Corrección aplicada**:
- `CDNAudioFile.h`: `HTTP_BUFFER_SIZE` de 14KB a **48KB** (~2.4s de audio
  por fetch en vez de ~0.7s) — menos fetches por minuto, menos
  oportunidades para que el jitter de red pegue. (`HTTP_BUFFER_SIZE`
  como miembro nombrado ya no existe — ver F84: pasó a escalar con el
  bitrate real del track, y luego se eliminó del todo a favor de
  `httpBuffer.size()`.)
- `BufferedAudioSink.h`/`.cpp`: el ring buffer de `startI2sFeed()` de
  32KB a **256KB** (~2.9s de colchón de PCM mono) — cubre toda la
  distribución medida salvo el pico extremo único de 3.5s. Cambiado de
  `xRingbufferCreate()` a `xRingbufferCreateWithCaps(..., MALLOC_CAP_SPIRAM
  | MALLOC_CAP_8BIT)` explícito — a este tamaño, dejarlo en el heap
  default hubiera comido una porción grande de la DRAM interna, ya
  escasa en este proyecto (F64/F66/F67).

**Por qué no afecta la latencia de pausa**: el `flush()` de F77 descarta
todo el contenido del ring buffer al pausar, así que un buffer más
grande no hace que pausar/despausar se sienta más lento — solo agranda
el colchón contra jitter de red durante reproducción normal.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre — es una asignación de RAM en runtime vía PSRAM, no cambia
el binario). El logging de timing (`CDN fetch: Xms...`) se dejó activo
para esta ronda, para poder confirmar en la próxima prueba que los
cortes efectivamente desaparecen (o al menos se reducen a solo el caso
extremo de varios segundos, si llegara a repetirse). Pendiente de
confirmar en hardware real.

### F84 — El tamaño de buffer de CDN asumía 160kbps fijo; ahora escala con el bitrate real del track (Bajo, aplicado)

Surgió al preguntarse: si se cambia `CONFIG_CSPOT_BITRATE` a 320 (Spotify
acepta 96/160/320), ¿el margen de F83 se mantiene? No — el tamaño de
buffer de F83 estaba fijado en **bytes** (48KB), no en segundos de audio.
A 320kbps esos mismos 48KB representan la mitad de tiempo de reproducción
(~1.2s en vez de ~2.4s) — el doble de pedidos al CDN por minuto contra el
mismo margen absoluto del ring buffer (2.9s, ese sí independiente del
bitrate porque es PCM ya decodificado a tasa de salida fija). Más pedidos
por minuto = más oportunidades para que el jitter de red pegue,
silenciosamente reduciendo la protección que F83 buscaba dar.

**Corrección aplicada** (`CDNAudioFile.h`/`.cpp`, `TrackQueue.cpp`):

- `CDNAudioFile`: nuevo parámetro `int bitrateKbps` en el constructor.
  El tamaño del buffer de lectura (`httpBuffer`) se calcula ahí mismo,
  como variable local, a partir de `bitrateKbps * 1000 / 8 *
  HTTP_BUFFER_SECONDS` (`HTTP_BUFFER_SECONDS = 2.4`, la misma cadencia
  que F83 ya validó a 160kbps), y se usa directo en
  `httpBuffer.resize(...)` — no quedó como miembro nombrado aparte:
  el único otro lugar que necesitaba ese tamaño (el header `Range:` del
  pedido HTTP, en `readBytes()`) consulta `httpBuffer.size()`
  directamente, ya que el buffer se redimensiona una sola vez y nunca
  más después. Un número menos que mantener sincronizado con el tamaño
  real del buffer.
- `TrackQueue.cpp`: nueva función `audioFormatBitrateKbps(AudioFormat)`
  que mapea el enum de formato (`OGG_VORBIS_96/160/320`,
  `MP3_96/160/256/320/160_ENC`) a kbps — con default 160 para cualquier
  valor no mapeado, mismo criterio que el propio texto de ayuda de
  `CONFIG_CSPOT_BITRATE` ("any other value falls back to 160 at
  runtime"). `QueuedTrack::getAudioFile()` ahora le pasa
  `audioFormatBitrateKbps(selectedFormat)` a `CDNAudioFile` — cada
  track usa el bitrate que efectivamente se le asignó (F60), no un
  valor global asumido.
- Se mantuvo la decisión de portabilidad de la sesión: `CDNAudioFile`
  recibe un `int` (kbps), no el enum `AudioFormat` — no necesita saber
  nada del esquema de metadata de Spotify, solo un número. El mapeo
  vive en `TrackQueue.cpp`, que ya conocía `AudioFormat` de antes.

A 96kbps el buffer queda en ~29KB; a 256kbps (`MP3_256`, podcasts) en
~77KB; a 320kbps en 96KB — todos apuntando a los mismos ~2.4s de margen
que F83 midió como suficientes a 160kbps.

**De dónde sale el `2.4` de `HTTP_BUFFER_SECONDS`** (no es un número
elegido a mano — es la cuenta inversa del `48KB` que F83 ya validó en
hardware real, a 160kbps):

```
48KB = 49 152 bytes
49 152 bytes × 8 bits/byte = 393 216 bits
393 216 bits / 160 000 bits/s (160kbps) ≈ 2.4576s ≈ 2.4
```

El orden histórico fue al revés de como se lee el código hoy: primero
F83 fijó el tamaño en bytes (48KB, buscado para que la mayoría de los
fetches entraran cómodos contra el margen del ring buffer, confirmado
con el log real de `CDN fetch: Xms`), y F84 tomó ese valor ya validado
y lo expresó en segundos para poder escalarlo a cualquier bitrate, en
vez de dejarlo fijo en bytes (que es justo lo que rompía el margen a
320kbps, como se explica arriba).

**Cómo y dónde recalibrar esto en el futuro**, si hiciera falta (por
ejemplo, si un remedido en hardware muestra que 2.4s ya no alcanza, o
que sobra margen y se puede achicar):

- **El único número a tocar es `HTTP_BUFFER_SECONDS`** en
  `CDNAudioFile.h` — todos los tamaños de buffer por bitrate
  (96/160/256/320kbps) se recalculan solos a partir de ahí, en el
  constructor de `CDNAudioFile` (`TrackQueue.cpp` no necesita cambios,
  solo mapea formato → kbps).
- **Ese número no es independiente del ring buffer de PCM** — el
  colchón de `BufferedAudioSink` (256KB, ~2.9s, ver F83) es lo que
  absorbe la espera mientras un fetch más grande está en vuelo. Si se
  sube `HTTP_BUFFER_SECONDS` bastante (pedidos más espaciados pero cada
  uno tarda más en completarse), conviene volver a chequear que el
  margen del ring buffer lo siga cubriendo cómodo — si no, hay que subir
  también `buf_size` en `BufferedAudioSink::startI2sFeed()`
  (`BufferedAudioSink.h`).
- **Cómo remedir**: el logging temporal de F83 (`CDN fetch: Xms, N
  bytes, reused=%d` en `CDNAudioFile.cpp`) sigue en el código — es el
  mismo mecanismo a reactivar/dejar activo para juntar una nueva
  distribución de tiempos reales si se sospecha que el margen actual ya
  no alcanza o que sobra de más.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real con
`CONFIG_CSPOT_BITRATE=320` que el comportamiento de buffering se
mantiene equivalente al de 160kbps.

## 11. Referencia: hallazgos investigando `librespot` (Rust) — julio 2026

[`librespot`](https://github.com/librespot-org/librespot) es la
implementación de referencia de Spotify Connect en Rust — más completa y
activamente mantenida que `cspot`, que en varias zonas (SPIRC, manejo de
metadata, descarga de audio) sigue un diseño más simple/antiguo. Se
investigó puntualmente durante la ronda de hardening de F75-F84, buscando
hallazgos de protocolo y arquitectura que valiera la pena tener
documentados — no como una lista de "cosas por portar", sino como
contexto para decisiones futuras. Ninguno de los puntos de acá está
implementado hoy en `cspot`, salvo donde se indica explícitamente.

### 11.1 — Protocolo: reemplazaron Mercury por "Dealer" (WebSocket)

Ver **F81** (arriba) para el detalle completo. Resumen: desde su v0.7.0
(agosto 2025), `librespot` dejó de usar Mercury (el protocolo binario
por TCP que `cspot` sigue usando para todo — SPIRC, suscripciones, etc.)
para el manejo de SPIRC/estado de Connect, migrando a "Dealer" — un
transporte por WebSocket (`wss://{host}:{port}/?access_token={token}`,
resuelto vía `apresolver().resolve("dealer")`, mismo mecanismo de
`ApResolve` que ya tenemos vendorizado, y autenticado con el mismo tipo
de access token que ya maneja `AccessKeyFetcher` vía Login5). Riesgo
arquitectónico a vigilar, no accionable hoy — Mercury nos sigue
funcionando sin problemas.

### 11.2 — Precedente real de endpoints legacy rotos sin aviso

[librespot-org/librespot#1623](https://github.com/librespot-org/librespot/issues/1623)/
[#1622](https://github.com/librespot-org/librespot/pull/1622) (mergeado
7 de noviembre de 2025): el endpoint legacy de metadata
`/metadata/4/{scope}/{id}` (HTTP REST) dejó de devolver datos de archivo
de audio, rompiendo la reproducción — el fix migró a
`/extended-metadata/v0/extended-metadata`. **No afecta directamente a
`cspot`**: `cspot` obtiene metadata vía Mercury (`hm://metadata/3/...`,
`TrackQueue.cpp:383`), un endpoint y transporte distintos — nunca migró
a la ruta HTTP que rompió acá. Sirve como precedente concreto de que
Spotify sí rompe backends legacy sin aviso — reforzó la decisión de
agregar visibilidad real de errores en nuestro propio camino HTTP más
frágil (`stepLoadCDNUrl()`/storage-resolve, ver F80) en vez de asumir
que "si compila y funciona hoy, va a seguir funcionando".

### 11.3 — Arquitectura de descarga de audio: prefetch en background, no implementado

La investigación con más detalle, hecha evaluando alternativas a F83/F84
para los cortes de audio periódicos. `librespot` (`audio/src/fetch/`) no
descarga de forma reactiva/bloqueante como `cspot` — tiene una
arquitectura productor/consumidor bastante más sofisticada:

- **Tarea de fondo dedicada** (`session.spawn(audio_file_fetch(...))`)
  que descarga de forma proactiva, desacoplada de quien decodifica —
  comunicadas por un canal (`mpsc::unbounded_channel::
  <StreamLoaderCommand>`) y un `Condvar` para que el consumidor se
  bloquee solo si realmente no hay datos todavía.
- **`RangeSet`**: estructura dedicada que trackea "lo pedido" vs. "lo ya
  llegado" por separado — más preciso que la ventana única
  (`lastRequestPosition`/`lastRequestCapacity`) que usa `CDNAudioFile`
  hoy. Pensado para manejar bien tanto reproducción secuencial como
  *seeks* arbitrarios con rangos no contiguos.
- **Tamaño de pedido adaptativo, no fijo**: en vez de un
  `HTTP_BUFFER_SIZE` constante (como el nuestro, F83/F84), miden el
  ping real de la red y ajustan cuánto pedir por adelantado —
  `<bytes pendientes> < PREFETCH_THRESHOLD_FACTOR (4.0) * <ping medido>
  * <bitrate>`, con un `maximum_assumed_ping_time` como techo para no
  descontrolarse. Se adapta solo a las condiciones de red del momento,
  en vez de sobredimensionar para el peor caso fijo.
- **Descargas concurrentes** (`download_slots: Semaphore`) — no es
  estrictamente secuencial como `CDNAudioFile::readBytes()`, lo cual
  esconde aún más la latencia individual de cada request.
- **Sin buffer del archivo completo**: siguen streameando con una
  ventana de prefetch acotada (`minimum_download_size`, default
  64KiB), no bajan el track entero de entrada.

**Por qué no se implementó**: el diseño equivalente para `cspot` fue
discutido en detalle (tarea de fetch propia dentro de `CDNAudioFile`,
`bell::Task`/`std::mutex`/`bell::WrappedSemaphore` para mantenerlo
portable, sin nada específico de ESP-IDF) pero se decidió pausarlo — la
mitigación más simple de F83/F84 (buffers más grandes, escalados por
bitrate) ya redujo bastante el problema real reportado, y no hay
evidencia todavía de que haga falta la inversión de una arquitectura de
prefetch completa. Queda esto como referencia de diseño ya pensada, para
retomar si F83/F84 resulta insuficiente con más uso acumulado — sin
necesidad de volver a investigar `librespot` desde cero.

### F85 — Crash real en hardware: corrupción de heap durante una ventana de fallos de red múltiples; heap poisoning activado para diagnosticar (Alto, investigación en curso)

Reportado por el usuario en hardware real, con los buffers de F83/F84 ya
funcionando bien (fetches de CDN sanos, 400-550ms, durante más de 10
minutos de reproducción previos al crash). El dispositivo se reinició
solo, con:

```
assert failed: remove_free_block tlsf_control_functions.h:373
  (next && "next_free field can not be null")
```

**Qué es esto**: TLSF (el allocator de heap de ESP-IDF) detectó que su
lista de bloques libres está corrupta — no es un error simple, es
corrupción de heap real (algo escribió fuera de los límites de un bloque
alojado, o hubo un doble-free/use-after-free en algún momento anterior).
El stack trace apuntaba a `bell::URLParser::parse()` (el parseo por
regex de una URL, dentro de `HTTPClient::Response::rawRequest()`,
llamado desde `CDNAudioFile::readBytes()`) — pero en corrupción de heap
**el lugar donde explota casi nunca es el lugar donde se originó**: el
allocator recién nota el daño la próxima vez que toca esa memoria, que
acá resultó ser una asignación completamente ajena al problema real.

**Lo que sí es un dato fuerte**: en la misma ventana exacta del crash,
la sesión de Mercury (una conexión TLS totalmente distinta a la del CDN)
también falló, dos veces seguidas:

```
MercurySession.cpp:69: Error while receiving packet: Error in read
PlainConnection.cpp:239: Closing socket...
MercurySession.cpp:346: Failed to send Mercury packet for hm://remote/user/.../: Error in write
MercurySession.cpp:346: Failed to send Mercury packet for hm://metadata/3/track/...: Error in write
```

Misma firma que ya se vio antes en esta sesión (investigación de F79):
un evento de red/WiFi más amplio afectando múltiples conexiones a la
vez, no un problema aislado de una sola.

**Se investigó F82 (el retry de `rawRequest()`, código nuevo, recién
ejercitado de verdad bajo fallos reales por primera vez) como sospechoso
directo** — se revisó `SocketBuffer::close()` (llamado dos veces
seguidas por el loop de retry) buscando un doble-free; tiene guarda
explícita contra doble-cierre (`if (internalSocket != nullptr &&
isOpen())`) y es seguro. No se encontró un bug concreto en ese código,
pero tampoco se puede descartar con certeza sin más herramientas —
corrupción de heap es notoriamente difícil de rastrear solo leyendo
código.

**Corrección aplicada — diagnóstico, no fix**: se activó
`CONFIG_HEAP_POISONING_COMPREHENSIVE` (`sdkconfig`, **no** en
`sdkconfig.defaults` — es deliberadamente temporal para esta ronda de
debug, no un default permanente del proyecto). Agrega guard bytes
alrededor de cada allocation y rellena la memoria liberada con un patrón
reconocible; con esto activado, la próxima vez que ocurra esta
corrupción, el propio allocator debería abortar **en el punto exacto**
donde se escribió fuera de límites, en vez de señalar a una víctima
inocente varias asignaciones después. Costo real de RAM/CPU — apropiado
para esta sesión de debug, **hay que desactivarlo** (volver a
`CONFIG_HEAP_POISONING_DISABLED=y`) una vez diagnosticado, no dejarlo en
producción.

**Verificado**: `idf.py fullclean` + `idf.py build` limpio, 0 errores,
mismo margen de flash (~52% libre). **Investigación en curso** —
pendiente de que el crash se repita con heap poisoning activo para
identificar el sitio real de la corrupción.

### F86 — `CDNAudioFile` confiaba ciegamente en el `Content-Length` del CDN para escribir en buffers de tamaño fijo, sin chequear que el servidor honrara el `Range` pedido (Alto, aplicado, mismo día que F85)

Sospecha concreta sobre F85, investigada antes de tener el resultado del
heap poisoning: en `readBytes()`, `lastRequestCapacity` se tomaba
directo de `httpConnection->contentLength()` (el header `Content-Length`
que devuelve el servidor) y se usaba sin validar como cantidad de bytes
a leer **hacia `httpBuffer`**, un buffer de tamaño fijo. El RFC 7233
(HTTP Range Requests) permite explícitamente que un servidor **ignore**
el header `Range` y responda `200 OK` con el recurso completo en vez de
`206 Partial Content` con el fragmento pedido — comportamiento legítimo
del protocolo, no un bug del servidor. Si eso pasara acá (más probable
durante un episodio de inestabilidad de red, como el que coincidió con
el crash de F85), `Content-Length` sería el tamaño del archivo restante
completo — potencialmente mucho mayor que `httpBuffer.size()` — y el
`.read()` subsiguiente escribiría fuera de los límites del buffer. Se
confirmó que esto es una práctica estándar a validar (no un caso raro
inventado): [`librespot`](https://github.com/librespot-org/librespot)
(`audio/src/fetch/receive.rs`) chequea explícitamente
`código != StatusCode::PARTIAL_CONTENT` y rechaza cualquier respuesta
que no sea `206` antes de confiar en nada de lo que llegó.

**Corrección aplicada** (`CDNAudioFile.cpp`), dos capas independientes,
no redundantes entre sí — defienden contra fallas distintas:

1. **Chequeo de status `206`** (mismo enfoque que `librespot`), en los
   tres range-requests del archivo (`openStream()`'s fetch de header y
   de footer, y `readBytes()`'s fetch del cuerpo): si el servidor no
   devuelve `206`, se rechaza la respuesta sin leer nada — `throw` en
   `openStream()` (ya cubierto por el try/catch de F75), `return 0` en
   `readBytes()` (mismo patrón que los demás fallos de esa función). Ya
   teníamos la herramienta para esto (`statusCode()`, agregado en F80),
   nunca aplicada acá.
2. **Clamp defensivo**, backstop que no depende de que el chequeo #1
   nunca falle: `lastRequestCapacity = std::min(contentLength(),
   httpBuffer.size())` — nunca se lee hacia el buffer más de lo que
   realmente mide, pase lo que pase con el valor que declaró el
   servidor.

Los `.read()` de `openStream()` hacia `header`/`footer` no tenían este
mismo riesgo de desborde (usan tamaños fijos, calculados por nuestro
propio código, no `contentLength()` directo) — pero sí el mismo riesgo
de **datos corruptos** si el servidor ignora el `Range` (leerían el
comienzo del archivo pensando que es el header, o el header pensando
que es el footer) — se les agregó el mismo chequeo de `206` por
consistencia, con el mismo costo casi nulo.

**No confirmado que sea la causa de F85** — es una hipótesis fundada
(RFC + precedente de `librespot`), reforzada por la correlación temporal
con la ventana de inestabilidad de red del crash, pero el heap poisoning
de F85 sigue siendo lo que puede confirmarlo con certeza. Vale como fix
independientemente: es la práctica estándar documentada por el propio
protocolo HTTP, no algo que dependa de que sea o no la causa de este
crash puntual.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real — tanto que no
rompe el camino normal (la enorme mayoría de las respuestas ya son
`206`, así que no debería cambiar nada en el caso sano) como, si F85 se
repite, si esto lo evitó.

### F87 — `BellLogger` (lo que usa `CSPOT_LOG` en prácticamente todo `cspot`) no tiene ningún lock: logs concurrentes se intercalan libremente, riesgo real de corrupción de heap (Alto, encontrado por inspección de código, corregido)

Segunda sospecha concreta sobre F85, encontrada revisando el propio
mecanismo de logging tras notar algo que venía pasando **en todos los
logs de esta sesión sin que se le prestara atención**: líneas de dos
tareas distintas apareciendo entremezcladas carácter por carácter en la
consola (ej. `"I MercurySession.cppI CDNAudioFile.cpp:217:..."`). Eso no
es solo estética — es la prueba visible de una falta de sincronización
real.

`BellLogger::debug()/error()/info()` (`BellLogger.h`) — lo que invoca
`CSPOT_LOG`, usado por prácticamente todo el motor (`TrackPlayer`,
`TrackQueue`, `MercurySession`, `SpircHandler`, `CDNAudioFile`) — arma
cada línea de log con **nueve llamadas separadas** a
`printf()`/`vprintf()`/`std::cout <<` (timestamp, color, nivel,
submódulo, nombre de archivo con hash de color, número de línea, el
mensaje en sí, reset de color, salto de línea), **sin ningún mutex, sin
`flockfile`, sin nada** que las mantenga juntas. Si dos tareas llaman a
`bellGlobalLogger->info(...)` (o `error`/`debug`) al mismo tiempo, esas
nueve llamadas de cada una se pueden intercalar arbitrariamente entre
sí — confirmado que `enableTimestampLogging(true)` está activo
(`cspot_connect_start()`), así que el camino de `std::cout`/
`std::put_time` en `printTimestamp()` se ejecuta en cada llamada, toda
la sesión.

**Por qué esto es candidato real para F85, no solo un problema
estético**: `printf`/`vprintf`/`std::cout` en un runtime de C++ típico
tienen estado interno de buffering (a menudo con memoria del heap) para
la propia `FILE`/`streambuf` de `stdout`. Acceso concurrente sin
sincronizar a ese estado interno, desde múltiples tareas de FreeRTOS
sin coordinación, es un mecanismo plausible de corrupción de heap real
— no solo de texto desordenado en la terminal. Esta sesión agregamos
bastante logging nuevo de alta frecuencia (el de `CDN fetch`, cada
~0.4-2s desde la tarea de `TrackPlayer`, en paralelo con lo que ya
logueaban `MercurySession`/`TrackQueue`/las tareas de UI), aumentando la
exposición real a esta carrera preexistente.

**Corrección aplicada** (`BellLogger.h`): un único `std::mutex
logMutex` (miembro de `BellLogger`), tomado al principio de `debug()`/
`error()`/`info()` (`std::lock_guard`) — la secuencia completa de nueve
llamadas de una línea de log ahora es atómica frente a cualquier otra
tarea llamando al mismo logger. `printFilename()` (llamado desde
adentro de esos tres métodos) no necesita su propio lock — ya corre
bajo el lock del método que lo invoca, sin riesgo de deadlock por
reentrancia (nunca se re-toma el mismo mutex). Portable — `std::mutex`
es C++ estándar, sin nada específico de ESP-IDF, consistente con que
`BellLogger.h` es código compartido por todas las plataformas que
soporta `bell`.

**No confirmado que sea la causa (o la única causa) de F85** — al igual
que F86, es una hipótesis fundada con evidencia real (el propio
intercalado visible, no inventado), no una certeza. F87 y F86 son
candidatos independientes, no mutuamente excluyentes — es
perfectamente posible que ambos contribuyan, o que ninguno sea la causa
raíz exacta y el heap poisoning de F85 señale algo distinto por
completo.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de flash
(~52% libre). Pendiente de confirmar en hardware real — que los logs ya
no aparezcan intercalados (efecto visible e inmediato de este fix,
verificable en el primer log que se capture) y, con más tiempo, si
F85 deja de repetirse.

### F88 — Feature: botón de repeat en la UI (antes ignorado, no persistía estado); repeat del track actual, no de la cola (Feature, aplicado)

El botón de repeat en `player_screen` no hacía nada — un click no
quedaba seleccionado ni afectaba la reproducción. La causa real estaba
en el motor, no en la UI: `SpircHandler::handleFrame()`'s case
`MessageType_kMessageTypeRepeat` (`SpircHandler.cpp`) llamaba
`this->notify()` sin más — un no-op que reenviaba el estado propio
(nunca actualizado) de vuelta al remoto, en vez de aplicar el valor de
repeat que el cliente remoto (o el propio dispositivo) acababa de
mandar. `TrackQueue` tampoco tenía ningún concepto de "repeat" — nada
que hacer con el flag aunque se guardara.

**Semántica elegida: repeat del track actual**, confirmada
explícitamente por el usuario. El protocolo SPIRC clásico que usa
`cspot` solo tiene un `bool repeat` (`spirc.proto`), a diferencia del
Connect-state moderno de `librespot`, que separa
`repeating_context`/`repeating_track` (`connect/src/state/options.rs`)
— con un único flag, `cspot` no puede representar "repetir toda la
cola" y "repetir el track actual" como dos estados independientes, así
que había que elegir uno.

**Referencia revisada antes de implementar**: el commit
[feelfreelinux/cspot@e326e44](https://github.com/feelfreelinux/cspot/commit/e326e44cd3b5d43606a159fa7790ffa5b60de236)
(upstream, no un fork) agrega soporte de repeat con exactamente esta
semántica (track actual) — `TrackQueue::consumeTrack()` devuelve el
mismo track sin avanzar mientras `shouldRepeat` esté activo. Se
verificó línea por línea antes de portar, siguiendo la misma práctica
que con los forks evaluados en F80/F82 — no se copia sin revisar. El
commit trae además un `setShuffle()` con un bug real (el flag queda
siempre en `true`, sin importar el parámetro recibido) y un
`stateMutex` que se declara pero nunca se usa — ninguno de los dos se
adoptó, porque shuffle está fuera de alcance de este cambio.

**Implementación** (motor, portable — sin nada específico de ESP-IDF):
- `PlaybackState::setRepeat(bool)` (`PlaybackState.h`/`.cpp`): marca
  `innerFrame.state.has_repeat = true` y fija `repeat`, para que el
  siguiente frame enviado (`notify()`) refleje el estado real.
- `TrackQueue::setRepeat(bool)` (`TrackQueue.h`/`.cpp`): fija
  `shouldRepeat` bajo `tracksMutex` (mismo lock que ya protege
  `preloadedTracks`, ver F78). `consumeTrack()`: si `shouldRepeat` está
  activo, devuelve `prevTrack` con `offset = 0` en vez de avanzar —
  mismo camino que "no hay track previo" más arriba en la función, así
  que dispara `trackLoaded()`/`PLAYBACK_START` de nuevo y resetea el
  tracking de posición para la repetición, igual que un track nuevo
  legítimo.
- `SpircHandler::setRepeat(bool)` (nuevo, `SpircHandler.h`/`.cpp`):
  aplica el estado a `playbackState` y `trackQueue`, notifica al
  remoto, y dispara un nuevo `EventType::REPEAT` para que la UI se
  entere. `handleFrame()`'s case de repeat ahora llama a esto en vez
  del `notify()` no-op original.
- `cspot_connect.h`/`.cpp`: `cspot_connect_set_repeat(bool)` (misma
  forma que `cspot_connect_play/pause/next/previous`, protegido por
  `spircMutex`), y `CSPOT_EVENT_REPEAT_ON`/`CSPOT_EVENT_REPEAT_OFF` en
  `cspot_event_t` — dispara en este dispositivo y en cualquier otro
  cliente Connect mostrando la misma sesión, por simetría con play/
  pause/next/previous.

**UI** (`components/ui/player_screen.cpp`): cuarto botón de transporte
(`LV_SYMBOL_LOOP`), a la derecha de "next" (`x_ofs = 130`, y = 268,
mismo estilo transparente que el resto). No usa `make_transport_btn()`
—igual que el botón de play/pause, necesita guardar el `lv_obj_t*` del
ícono (`s_repeat_icon`) para poder cambiar su color al togglear, algo
que el helper no expone. Color verde (`0x1DB954`, mismo verde de Spotify
que ya usa el ícono de play/pause y la barra de progreso) cuando el
repeat está activo, gris (`0x999999`) cuando no. El click no asume el
nuevo estado localmente — encola el comando opuesto al `s_is_repeat`
actual (mismo patrón que `play_pause_btn_cb`), y el ícono solo cambia
de verdad cuando llega el evento `CSPOT_EVENT_REPEAT_ON`/`_OFF` desde
el motor, sea por el click propio o por un cambio remoto.

**Verificado**: `idf.py build` limpio, 0 errores nuevos (los warnings
de LVGL `-Wdeprecated-enum-enum-conversion` son preexistentes en todo
el archivo, ya suprimidos vía `-Wno-error` en
`components/ui/CMakeLists.txt`), mismo margen de flash. Pendiente de
confirmar en hardware real: que el click quede visualmente marcado, que
la repetición del track realmente ocurra al llegar al final, y que un
cambio de repeat desde otro cliente Connect también se refleje acá.

**Nota, sin resolver todavía**: cuando se implemente el protocolo
"Dealer" (WebSocket, ver F81) esto se puede hacer más granular
(repeat-context vs. repeat-track por separado, como ya hace
`librespot` moderno) — confirmado con el usuario como plan a futuro,
no parte de este cambio.

**Actualización (F92)**: la semántica "repeat del track actual"
elegida acá resultó ser la incorrecta para este tipo de dispositivo —
un cliente real de Spotify conectado por SPIRC clásico solo expone
"repetir la cola actual" para este botón, no repeat por track (eso es
exclusivo de clientes con el protocolo moderno). Rediseñado en F92 a
repeat de cola. La sección "Implementación" de acá arriba describe el
diseño original (track actual), ya reemplazado — se deja como registro
histórico de la decisión y por qué se tomó, no como el comportamiento
actual.

### F89 — Track que falla al cargar: la UI mostraba carátula/nombre en blanco y una duración absurdamente alta en el track siguiente (Alto, encontrado en hardware real, corregido)

Reportado por el usuario en hardware real (build anterior a F85-F88):
un track falló su carga (timeout de 5s esperando metadata/CDN URL) y
`TrackPlayer` saltó directo al siguiente sin crashear — correcto — pero
la pantalla, en el track que arrancó a sonar, mostró carátula vacía,
nombre de track vacío, y una duración con un número absurdamente alto.
Se "recuperó" con el track siguiente.

**Causa raíz**: `TrackQueue` mantiene la posición "oficial" de la cola
de dos formas independientes que normalmente avanzan en lockstep, pero
no en este caso:

- `TrackPlayer::runTask()` camina la cola con su propio contador
  posicional: `consumeTrack(track, offset)` busca `track` (el último
  reproducido) dentro de `preloadedTracks` y devuelve el siguiente
  índice — sin tocar `preloadedTracks` ni `currentTracksIndex`.
- `preloadedTracks[0]`/`currentTracksIndex` (la "cabeza oficial", la
  que usan `SpircHandler::notifyAudioReachedPlayback()` y el estado que
  se manda a otros clientes Connect) solo avanza cuando algo llama a
  `TrackQueue::skipTrack()` — un paso a la vez.

En reproducción normal ambos avanzan de a un track por vez y quedan
sincronizados. Pero cuando un track falla (`TrackPlayer.cpp:172`,
`track->state != READY` tras el timeout de 5s), `TrackPlayer` hace su
`continue` y en la siguiente vuelta del loop su caminata posicional
salta **dos** posiciones de una sola vez (la que falló + la que sí
carga) — sin que nada le avise a `TrackQueue` que la fallida quedó
atrás. `preloadedTracks[0]` sigue apuntando al track fallido.

Cuando el audio del track nuevo llega a reproducirse,
`notifyAudioReachedPlayback()` (`SpircHandler.cpp:88-127`) confía
ciegamente en `preloadedTracks[0]` como "lo que está sonando ahora" y
manda `sendEvent(TRACK_INFO, currentTrack->trackInfo)` con la info del
**track fallido**, no del que realmente está sonando. Y esa
`TrackInfo` (`TrackQueue.h:25-27`, `uint32_t duration, number,
discNumber;` sin inicializador) nunca llegó a poblarse — el track
fallido nunca completó ni siquiera `stepLoadMetadata()` (no hay log de
"Track name: ..." para él) — así que sus campos numéricos quedan con
lo que sea que había en esa memoria, no en cero. Eso explica los tres
síntomas juntos: nombre y carátula vacíos (strings default-construidos,
sí quedan vacíos) y duración con un número sin sentido (lectura de un
`uint32_t` nunca inicializado, no una duración real).

La "recuperación" en el track siguiente no es una recuperación real:
`notifyAudioReachedPlayback()` solo avanza `preloadedTracks[0]` de a
una posición por llamada (vía su propio `skipTrack(NEXT, false)`
interno), así que el desfase de dos posiciones se reduce a uno — pasa
de reportar el track fallido (info basura) a reportar el track
*anterior* al que realmente suena (info válida, pero de otro track). El
desfase de una posición queda instalado para el resto de la cola hasta
el próximo `Load`/`Replace` de Spirc, simplemente deja de ser visible
porque ahora es info válida en vez de basura.

**Corrección aplicada** (`TrackPlayer.cpp`, un solo call site): en la
rama de fallo, antes de `eofCallback()` + `continue`, se agregó
`trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT, false)` — el
mismo call que ya usa `notifyAudioReachedPlayback()` para su propio
avance de a un paso. Así, cada salto posicional de `TrackPlayer`
(fallido o no) tiene su contraparte de un paso en la cabeza oficial de
`TrackQueue`, y quedan sincronizados de nuevo. `expectNotify=false`
(igual que en `notifyAudioReachedPlayback()`) porque esto no es un skip
iniciado por el usuario — no corresponde resetear la posición ni
marcar `notifyPending`. Efecto colateral correcto: como
`skipTrack()` también actualiza
`playbackState->innerFrame.state.playing_track_index`, esto además
corrige el estado que se manda a otros clientes Spotify Connect
mostrando la misma sesión — no era solo un problema cosmético local.

No se tocó `consumeTrack()`/el mecanismo de doble tracking en sí — la
corrección es puntual sobre el único lugar donde los dos contadores se
podían desincronizar (un salto de más de una posición sin aviso a
`TrackQueue`), no un rediseño de cómo se trackea la cola.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de
flash. Pendiente de confirmar en hardware real que, ante un fallo de
carga real, la pantalla ya no muestre el estado en blanco/duración
absurda.

### F92 — Repeat: la semántica elegida en F88 (repeat del track actual) era la incorrecta — se rediseñó a repeat de la cola completa (Feature, corregido)

Primera prueba en hardware de F88: el usuario activó repeat desde un
cliente real de Spotify y, al terminar el track, el cliente mostró que
había pasado al siguiente — inicialmente diagnosticado acá como un bug
de sincronización entre `TrackPlayer` y `SpircHandler::
notifyAudioReachedPlayback()` (primer intento de F92, ya descartado).
El usuario corrigió esa lectura: no era un bug de sincronización — el
botón de repeat, cuando `cspot` está conectado como dispositivo
Connect vía SPIRC clásico, es la app de Spotify la que solo ofrece
"repetir la cola actual" para ese botón. El repeat por-track es una
opción que la app solo expone a dispositivos que hablan el protocolo
moderno (Dealer/Connect-state, con `repeating_context`/
`repeating_track` separados — ver F81) — con SPIRC clásico, el único
significado que el cliente le da a ese botón es repeat de cola. La
semántica "repeat del track actual" que F88 había implementado
(confirmada explícitamente en su momento, antes de tener esta
evidencia real) no correspondía a lo que el botón realmente hace para
este tipo de dispositivo.

**Se revirtió** la implementación de repeat-por-track de F88:
- `TrackQueue::consumeTrack()`: se sacó la rama `if (shouldRepeat) {
  offset = 0; return prevTrack; }`.
- `SpircHandler::notifyAudioReachedPlayback()`: se sacó la rama
  agregada en el primer intento de F92 (la que evitaba el
  `skipTrack(NEXT)` cuando había repeat) - ya no aplica, vuelve al
  código original de antes de F88/del primer F92.

**Rediseño: repeat de cola completa** — al llegar al final de la cola
con repeat activo, se reinicia desde el primer track en vez de
terminar la reproducción:
- `TrackQueue::isRepeatingContext()` (se mantiene del primer intento de
  F92 - getter público bajo `tracksMutex`, sigue haciendo falta).
- `TrackQueue::restartFromBeginning()` (nuevo, `TrackQueue.h`/`.cpp`):
  resetea `currentTracksIndex` a 0 (también
  `playbackState->innerFrame.state.playing_track_index`, para que el
  estado que se manda a otros clientes Connect quede consistente),
  limpia `preloadedTracks` y vuelve a encolar desde el track 0
  (`queueNextTrack(0)`) — mismo patrón que la rama `initial` de
  `updateTracks()`, pero sin releer `playbackState->remoteTracks` (la
  lista de tracks no cambió, no hace falta volver a copiarla). Fija
  `notifyPending = true` para que la próxima llamada a
  `notifyAudioReachedPlayback()` trate el track 0 como "posición ya
  conocida (0)" en vez de intentar un `skipTrack(NEXT)` normal — mismo
  mecanismo que ya usa `updateTracks(initial)` para una carga fresca.
  Llama a `playableSemaphore->give()` para despertar a `TrackPlayer`
  de inmediato en vez de esperar hasta 300ms (`TrackPlayer.cpp`, el
  polling de `playableSemaphore->twait(300)` que corre cuando
  `isFinished()` queda en `true`).
- `SpircHandler`'s `EOFCallback` (constructor de `SpircHandler`,
  `SpircHandler.cpp`): en vez de mandar `EventType::DEPLETED`
  incondicionalmente cuando `trackQueue->isFinished()`, ahora chequea
  `trackQueue->isRepeatingContext()` primero — si está repitiendo, llama
  a `restartFromBeginning()` en vez de terminar la reproducción.

No hizo falta tocar `TrackPlayer.cpp` en absoluto — la reanudación
pasa completamente por el mecanismo que ya usa `TrackQueue`/
`SpircHandler` para una carga inicial (`notifyPending` +
`playableSemaphore`), sin ningún camino nuevo de índices/wraparound
dentro de la ventana de precarga (`preloadedTracks`, `MAX_TRACKS_
PRELOAD=3`) — evita el riesgo de tener que hacer aritmética de módulo
sobre esa ventana, que hubiera sido bastante más propenso a bugs
sutiles.

**Renombrado a `*RepeatContext*` en todo el call chain** (pedido
explícito del usuario, para dejar el nombre sin ambigüedad de cara a un
futuro `repeating_track` cuando se implemente Dealer — ver F81):
`PlaybackState::setRepeat`→`setRepeatContext`,
`TrackQueue::shouldRepeat`→`shouldRepeatContext`,
`TrackQueue::setRepeat`→`setRepeatContext`,
`TrackQueue::isRepeating`→`isRepeatingContext`,
`SpircHandler::setRepeat`→`setRepeatContext`,
`SpircHandler::EventType::REPEAT`→`REPEAT_CONTEXT`,
`cspot_connect_set_repeat`→`cspot_connect_set_repeat_context`,
`CSPOT_EVENT_REPEAT_ON/OFF`→`CSPOT_EVENT_REPEAT_CONTEXT_ON/OFF`, y en
`player_screen.cpp`: `s_is_repeat`→`s_is_repeat_context`,
`ControlCommand::kRepeatOn/kRepeatOff`→`kRepeatContextOn/
kRepeatContextOff`. `s_repeat_icon`/`repeat_btn_cb`/
`set_repeat_icon_locked` no se tocaron - son nombres de UI genéricos
(el ícono/botón en sí), no dependen de qué tipo de repeat controlan.
De paso se corrigió un comentario desactualizado en
`PlaybackState.h`/`TrackQueue.h` que todavía decía "repeat-current-track".

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de
flash. Pendiente de confirmar en hardware real: que al llegar al final
de la cola con repeat activo, reinicie desde el primer track en vez de
detenerse, y que el cliente vea el `playing_track_index`/`TRACK_INFO`
correctos en cada vuelta.

### F93 — Crash real en hardware, con causa raíz confirmada por heap poisoning: `MercurySession::shanConn` (y todo su estado interno) sin ningún lock, usado desde múltiples tareas a la vez (Crítico, encontrado en hardware real, corregido)

Primera corrida completa "toda la noche" con F85 (heap poisoning) + F86 +
F87 activos. A diferencia de la corrupción de heap original de F85 (que
señalaba a una víctima inocente, sin sitio exacto), esta vez el crash
tuvo un backtrace limpio y accionable — exactamente para lo que se
activó el heap poisoning:

```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
EXCVADDR: 0xfefeff02   <- 0xFE es el patrón de relleno de heap poisoning
                          para memoria ya liberada
```

Backtrace: `TrackPlayer` (su propia tarea) → `pcmWrite()` →
`SpircHandler::notifyAudioReachedPlayback()` → `notify()` → `sendCmd()`
→ `MercurySession::execute()`/`executeSubscription()` →
`ShannonConnection::sendPacket()` → crash exactamente en el
**destructor de un `std::scoped_lock`**, haciendo `unlock()` de un
mutex que vivía dentro de un objeto `ShannonConnection` ya destruido.

**Causa raíz**: en el mismo instante, el log muestra `MercurySession.cpp:69:
Error while receiving packet: Error in read` seguido de `Closing
socket...` — `MercurySession::runTask()` corre en su **propia tarea
dedicada** (`mercury_dispatcher`); ante un error de lectura, llama a
`reconnect()`, que hacía `this->shanConn = nullptr;` sin ningún lock,
destruyendo el `ShannonConnection` actual (mutex interno incluido).
`Session::shanConn` (`Session.h`, público, sin protección) se lee y se
usa directo (`this->shanConn->sendPacket(...)`) desde
`executeSubscription()`/`requestAudioKey()`, invocables **desde
cualquier tarea** — en este caso, la de `TrackPlayer`, en el momento
exacto en que `mercury_dispatcher` lo estaba destruyendo. Exactamente
el gap que **F62 ya había señalado como conocido** ("no cubre
pcmWrite()'s notifyAudioReachedPlayback/Ended"), ahora confirmado con
un crash real, no solo teórico.

**Investigando más, el problema resultó más amplio que un solo
puntero**: `MercurySession` tiene además tres `std::unordered_map`
(`callbacks`, `subscriptions`, `audioKeyCallbacks`), dos contadores
(`sequenceId`, `audioKeySequence`) y un `Header tempMercuryHeader`
reutilizado — todos mutados por `executeSubscription()`/
`requestAudioKey()` (cualquier tarea) **y** leídos/borrados por
`handlePacket()`/`failAllPending()` (la tarea de `CSpotConnectPlayer`,
vía el loop de `runSessionInner()`), sin ningún lock tampoco. Es la
misma clase de bug que causó el crash de `shanConn`, sobre más
variables — un `unordered_map::insert()` en una tarea a la vez que
`::find()`/`::erase()` en otra es comportamiento indefinido, con más
probabilidad de corromper silenciosamente (un callback que nunca
dispara, uno que dispara con datos de otro) que de crashear al toque
como pasó con `shanConn`.

**Corrección aplicada** — un mutex de sesión, aplicado de forma
consistente, nunca sostenido durante I/O de red ni durante la
ejecución de un callback:
- `Session::shanConnMutex` (nuevo, protegido, `Session.h`/`.cpp`) — protege
  `conn`/`shanConn` en sí (no lo que apuntan — `ShannonConnection` ya
  tiene sus propios `writeMutex`/`readMutex` para eso).
  `Session::getShanConn()` (nuevo): toma el lock solo para copiar el
  `shared_ptr` — el objeto sigue vivo (por refcounting) aunque
  `reconnect()` lo reemplace un instante después.
- `Session::connect()`: reescrito para hacer todo el handshake (varios
  round-trips de red bloqueantes) sobre variables locales, y recién
  asignar `conn`/`shanConn` bajo el lock al final — sostener el lock
  durante todo el handshake hubiera hecho que cualquier `getShanConn()`
  de otra tarea colgara por segundos en vez de fallar rápido como hace
  hoy.
- `MercurySession::reconnect()`: el `nullptr` de `conn`/`shanConn` pasa
  a estar protegido por el mismo lock.
- `MercurySession::sessionMutex` (nuevo, privado) — protege
  `callbacks`/`subscriptions`/`audioKeyCallbacks`/`sequenceId`/
  `audioKeySequence`/`tempMercuryHeader`/`countryCode`.
  `executeSubscription()`/`requestAudioKey()`: el lock cubre solo la
  construcción del paquete + registro del callback (rápido, todo en
  memoria) — se libera antes de mandar por la red.
  `handlePacket()`/`failAllPending()`: el lock cubre solo la
  búsqueda/borrado en el mapa — el callback en sí se invoca **fuera**
  del lock (evita bloquear a otras tareas por el tiempo que tarde un
  callback, y evita un deadlock si algún callback alguna vez volviera a
  llamar a `execute()` desde adentro).
- `MercurySession::disconnect()`: de paso, un null-deref latente
  arreglado (`conn->close()` sin chequear si `conn` podía ser
  `nullptr` durante un reconnect en curso).

**¿Degrada el servicio?** No debería — el lock nuevo nunca se sostiene
durante una llamada de red ni durante la ejecución de un callback,
solo durante operaciones en memoria (copiar un `shared_ptr`, insertar
en un mapa, incrementar un contador) que tardan microsegundos. La
serialización real de la I/O de red ya la hacía `ShannonConnection`
por su cuenta (`writeMutex`/`readMutex`) desde antes de este cambio.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de
flash. Pendiente de confirmar en hardware con otra corrida larga que
el crash no se repita.

### F94 — Track al final de la ventana que falla al cargar: el dispositivo quedaba reintentando el mismo track roto para siempre, cada 5s (Alto, encontrado en hardware real, corregido)

Después de aplicar F93, primera prueba en hardware mostró un problema
distinto: al conectar, ningún track sonaba — el log mostraba
`Track failed to load, skipping it` en loop, cada ~5 segundos,
indefinidamente, sin recuperarse nunca. Se investigó en detalle antes
de tocar código para descartar que fuera una regresión de F93 (no lo
era — se rastreó la cadena completa de logs y el mecanismo es
independiente de los cambios de locking).

**Causa raíz**: el cliente mandó un `Load` con `playing_track_index=10`
— y ese resultó ser el **último índice** de la ventana corta de tracks
que el cliente envió (`playbackState->remoteTracks`, no
necesariamente el playlist completo). La metadata de ese track
específico falló (`QueuedTrack::stepLoadMetadata()`'s callback,
`TrackQueue.cpp`, `res.parts.size() == 0` — el servidor respondió pero
sin datos; el porqué de esa respuesta vacía queda para investigar
aparte). Encadenado:

1. `TrackPlayer.cpp:179` (F89) llama `skipTrack(NEXT)` al fallar el
   track — pero como es el último índice de la ventana, `skipTrack`
   no puede avanzar (`currentTracks.size() > currentTracksIndex + 1`
   es falso) y no hace nada.
2. `eofCallback()` ve `isFinished()==true` (seguimos en el último
   índice) y repeat está apagado → dispara `EventType::DEPLETED` →
   `SpircHandler::notifyAudioEnded()` → `notify()` +
   `trackPlayer->resetState(true)`.
3. `resetState(true)` pone `pendingReset=true` → el loop de
   `TrackPlayer` resetea `track=nullptr` y vuelve a llamar
   `consumeTrack(nullptr, offset)`.
4. `TrackQueue::consumeTrack()`'s rama "sin track previo" devolvía
   `preloadedTracks[0]` **sin chequear su estado** — y como nunca se
   pudo sacar de la cabeza (paso 1), seguía siendo el mismo track
   `FAILED` de siempre. Vuelta a empezar: mismo track, mismo fallo,
   mismo `DEPLETED`, cada 5 segundos (el timeout de
   `loadedSemaphore->twait(5000)`), para siempre - con el efecto
   colateral de mandar un `notify()` (tráfico Mercury real) en cada
   vuelta, indefinidamente, mientras el dispositivo siguiera conectado.

**Corrección aplicada** (`TrackQueue::consumeTrack()`,
`TrackQueue.cpp`): la rama "sin track previo" ahora chequea el estado
antes de devolver la cabeza — si `preloadedTracks[0]->state ==
State::FAILED`, devuelve `nullptr` en vez del track roto. De paso se
agregó el chequeo de `preloadedTracks.empty()` que faltaba antes del
acceso a `[0]` (no crasheaba en la práctica porque siempre había algo
encolado, pero era un acceso fuera de rango latente sin chequear).

Con este fix, `TrackPlayer` recibe `newTrack == nullptr` y toma su
camino ya existente para "nada para reproducir todavía"
(`BELL_SLEEP_MS(100); continue;`) — sin loggear el error, sin volver a
llamar `skipTrack`/`eofCallback`/`notify()`. El dispositivo queda en
un poll silencioso de ~100-150ms (mismo patrón que ya usa esta función
en otros puntos para "todavía no hay track"), sin tráfico de red,
hasta que el cliente mande un `Load` nuevo con más tracks o el usuario
salte manualmente — el dispositivo no tiene forma de "autocurar" un
track roto por su cuenta, igual que pasaría con un speaker Connect
real.

**No resuelto todavía, a propósito**: por qué la metadata de ese track
específico vuelve vacía. Es una investigación aparte, pendiente.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de
flash. Pendiente de confirmar en hardware que, ante este escenario, el
dispositivo deje de repetir el ciclo de fallo cada 5s y quede en
silencio esperando un `Load` nuevo.

### F95 — Al fallar el track y quedar en silencio (F94), el cliente seguía mostrando la barra de progreso avanzando como si sonara (Medio, encontrado en hardware real, corregido)

Confirmado en hardware: F94 resolvió el loop infinito (ya no reintenta
el track roto cada 5s) — pero el cliente seguía mostrando la barra de
progreso avanzando sola, aunque el dispositivo estuviera efectivamente
en silencio, sin audio.

**Causa raíz**: `SpircHandler::notifyAudioEnded()` (llamado desde el
mismo camino de F94 - `EventType::DEPLETED`, tanto en fin de cola
genuino como en fallo de carga) solo hacía
`playbackState->updatePositionMs(0); notify();` — nunca actualizaba
`innerFrame.state.status`. Ese campo había quedado en
`PlayStatus_kPlayStatusPlay` desde que `handleFrame()`'s case
`kMessageTypeLoad` lo puso en `Playing` al recibir el `Load`
(`SpircHandler.cpp:218`), **antes** de siquiera intentar cargar el
track. El `notify()` de `notifyAudioEnded()` mandaba entonces
`status=Play, position=0` al cliente — que lo interpreta como "sigue
sonando, arrancó de nuevo desde 0" y anima la barra por su cuenta.
Antes de F94 esto era menos visible (el ciclo de reintento cada 5s
generaba un `notify()` nuevo constantemente, nunca dejaba que el
cliente extrapolara por mucho tiempo); con F94 aplicado, el
dispositivo queda en silencio indefinidamente, así que la barra
avanzando sola se volvió mucho más notoria y engañosa.

**Corrección aplicada** (`SpircHandler::notifyAudioEnded()`,
`SpircHandler.cpp`): agrega
`playbackState->setPlaybackState(PlaybackState::State::Paused)` antes
del `updatePositionMs(0)` existente — orden importa: el caso `Paused`
de `setPlaybackState()` recalcula la posición en base al tiempo
transcurrido desde la última medición, así que si se llamara
*después* del `updatePositionMs(0)`, pisaría el 0 explícito con un
valor viejo. Con el orden correcto, el frame que recibe el cliente
queda `status=Pause, position=0` — consistente con lo que realmente
pasa.

No se tocó `State::Stopped` (existe en el enum pero su `case` en
`setPlaybackState()` es un no-op, no hace nada) - `Paused` es el único
estado que realmente comunica "no está sonando" en este protocolo.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de
flash. Pendiente de confirmar en hardware que la barra de progreso del
cliente deje de avanzar sola cuando el dispositivo no tiene nada para
reproducir.

### F96 — Regresión real de F93: el reordenamiento de la aliasing GET→SEND rompía el campo `method` del header, y el servidor devolvía metadata vacía para *todos* los tracks (Crítico, regresión propia, encontrado y aislado por el usuario con `git stash`, corregido)

El usuario reportó un segundo escenario, mucho peor que F94: en vez de
un solo track fallando, **todos** los tracks de la cola fallaban en
cascada, uno tras otro, sin parar — el síntoma de "metadata vacía"
(`res.parts.size() == 0`) que se había visto antes, pero generalizado.
Pidió específicamente reverificar el fix de F93 antes de seguir.

**Cómo se aisló**: el usuario hizo dos `git stash` — el primero
(`fix-memo`) con "los últimos cambios", el segundo (`repeat`) con el
resto puesto en staged. El segundo dejó el árbol de trabajo limpio
(`git status` sin cambios, exactamente en `HEAD`). Comparando el
contenido de ambos stashes (`git diff stash@{0} stash@{1}`), quedó
claro que `stash@{1}` (`fix-memo`) contenía **todo** lo de la sesión
(F75-F95, 21 archivos) y `stash@{0}` (`repeat`) era un subconjunto sin
`Session.h`/`Session.cpp`/`MercurySession.h` — exactamente los archivos
de F93. Es decir: con F93 aplicado el bug aparecía: sin él,
desaparecía. Aislado con evidencia real, no por descarte.

**Causa raíz**: en `MercurySession::executeSubscription()`, el
reordenamiento hecho en F93 (mover la construcción del header a
*adentro* del bloque bajo `sessionMutex`) sin querer también movió la
línea `if (method == RequestType::GET) { method = RequestType::SEND; }`
de **después** del `pbPutString(RequestTypeMap[method],
tempMercuryHeader.method)` a **antes**. El código original dependía
exactamente de ese orden: el campo de texto `method` del header Mercury
debía llevar `"GET"` (lo que el servidor usa para decidir si debe
devolver el cuerpo de la respuesta), mientras que el aliasing GET→SEND
es *solo* para el byte de tipo de paquete de más abajo (que sí es
idéntico en la red, según el comentario original) — dos cosas distintas
que el código original mantenía separadas por el orden de las líneas,
sin ningún comentario que lo hiciera obvio. Con el orden invertido,
**todo** pedido de metadata (`type GET`) terminaba mandando `"SEND"` en
el header — el servidor lo trataba como un mensaje de solo-escritura
(como `notify()`) y respondía sin cuerpo. Explica perfectamente por qué
en los logs el tráfico `SEND`/`SUB` (Hello, notify, pause, suscripción)
siempre funcionó bien durante toda la sesión — nunca dependió de este
orden — mientras que *todo* pedido `GET` fallaba sistemáticamente.

**Corrección aplicada** (`MercurySession.cpp`,
`executeSubscription()`): se captura `headerMethodStr =
RequestTypeMap[method]` **antes** del aliasing GET→SEND, y se usa esa
copia (no `RequestTypeMap[method]` releído después del alias) para
`pbPutString(headerMethodStr, tempMercuryHeader.method)`. El resto de
F93 (el lock, `getShanConn()`, la separación de `assignedSequenceId`)
queda igual — el bug estaba puntualmente en estas dos líneas
reordenadas, no en el mecanismo de locking en sí.

**Lección**: al reordenar código alrededor de un lock, revisar con
cuidado que ninguna línea reordenada dependa implícitamente del orden
original con otra — acá dos conceptos (el texto del header vs. el byte
de tipo de paquete) compartían la misma variable `method`, mutada
in-place, sin que el código original lo señalara con un comentario.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de
flash, confirmado que F92 (repeat-context) y el resto de F75-F95 siguen
intactos después de restaurar el stash. Pendiente de confirmar en
hardware que la metadata vuelva a cargar normalmente para toda la cola.

### F97 — Cosmético: al conectar, el cliente mostraba "reproduciendo" (barra corriendo) antes de que el audio realmente arrancara, y después corregía de golpe hacia atrás (Bajo, aplicado)

Reportado por el usuario: al conectar el dispositivo, el cliente
muestra la barra de progreso avanzando de inmediato, como si ya
estuviera sonando — pero el audio recién arranca unos segundos después
(el tiempo que tarda en bajar los primeros chunks del CDN). Cuando por
fin arranca, el cliente "salta hacia atrás" y corrige la posición.
Cosmético, no afecta la reproducción en sí, pero confunde.

**Causa raíz**: `SpircHandler::handleFrame()`'s case
`MessageType_kMessageTypeLoad` (`SpircHandler.cpp`) marcaba
`playbackState->setPlaybackState(PlaybackState::State::Playing)` y
mandaba `notify()` **inmediatamente** al recibir el frame `Load` —
antes de que `TrackQueue`/`TrackPlayer` siquiera empezaran a pedir la
metadata, la audio key o la URL del CDN del track. El cliente recibe
"estoy reproduciendo desde la posición X" y empieza a extrapolar/animar
la barra por su cuenta en tiempo real. Recién cuando el track
realmente carga y arranca el audio (`trackLoadedCallback`, el lambda
del constructor de `SpircHandler`, disparado desde
`TrackPlayer::runTask()` una vez que el stream abre de verdad) se manda
un segundo `notify()` con la posición real — que, al haber pasado
varios segundos desde el primer aviso optimista, ya no coincide con lo
que el cliente venía mostrando, y salta para corregir.

**Corrección aplicada**: en vez de `State::Playing`, el frame `Load`
ahora marca `State::Loading` — que en este protocolo (`PlaybackState::
setPlaybackState()`) se traduce a `status = PlayStatus_kPlayStatusPause`
(no existe un estado "Loading" separado en el wire format de SPIRC
clásico, solo Play/Pause/Stop). El cliente ve "pausado en la posición
X" en vez de "reproduciendo" — no anima nada — hasta que
`trackLoadedCallback` cambia a `Playing` de verdad, con la posición
correcta, sin necesidad de corregir después. Un detalle de orden:
`State::Loading` resetea `position_ms` a 0 como efecto colateral, así
que el `updatePositionMs(remoteFrame.position)` explícito tiene que ir
**después**, no antes, para que la posición correcta quede puesta.

No afecta la UI propia del dispositivo (`player_screen.cpp`) — esa se
actualiza vía `EventType::PLAYBACK_START`/`PLAY_PAUSE`, que solo se
disparan desde `trackLoadedCallback` (la carga real), nunca desde el
`Load` frame directo — este bug era puramente sobre lo que veía el
cliente remoto.

**Verificado**: `idf.py build` limpio, 0 errores, mismo margen de
flash. Pendiente de confirmar en hardware que el cliente ya no muestre
la barra avanzando antes de que el audio arranque de verdad.

### F98 — `Crypto.cpp` (código de `bell`, se supone portable) usaba `esp_log.h`/`ESP_LOGE` en vez del logger portable; de paso, `BELL_LOG` (el macro que le correspondía) estaba roto (Bajo, aplicado)

Auditando `Crypto.cpp` por pedido del usuario: era el único archivo en
todo el árbol portable `bell`/`cspot` (fuera de `platform/esp/`, que
es específico de plataforma por diseño) que incluía `esp_log.h` y
llamaba `ESP_LOGE()` directo, en vez de usar el logger portable como
el resto del código — probablemente un descuido al portar este
archivo durante el hardening de mbedTLS 4.0 (F2).

**Complicación al arreglarlo**: `Crypto.cpp` vive en `bell/`, una capa
por debajo de `cspot/` (`cspot` depende de `bell`, no al revés) — usar
`CSPOT_LOG`/`Logger.h` (que vive en `include/`) hubiera sido una
dependencia hacia arriba, violando esa capa. `bell` ya tiene su propio
macro para esto, `BELL_LOG` (`BellLogger.h`), que no depende de nada de
`cspot`.

**Bug encontrado de paso**: `BELL_LOG` tal como estaba definido
(`bell::bellGlobalLogger->type(__FILE__, __LINE__, __VA_ARGS__)`)
nunca se llama con solo `(type, "formato", args...)` en ningún lugar
real del código — `AbstractLogger::debug/error/info` requieren un
parámetro `submodule` (`std::string`) antes del formato. El único
call site real que existía, en `EncodedAudioStream.cpp`, ya lo sabía y
pasaba el tag como su propio primer argumento variádico
(`BELL_LOG(info, TAG, "Error en frame...", ...)`) — el macro en sí
está bien diseñado así (no hardcodea un tag, cada archivo pasa el
suyo), simplemente nadie lo había usado desde `Crypto.cpp` antes para
notar que hacía falta pasarlo explícitamente. Intenté "arreglar" el
macro para que hardcodee un tag fijo, pero eso rompía el call site
existente de `EncodedAudioStream.cpp` — revertido; el macro estaba
bien, mi primer intento de uso en `Crypto.cpp` estaba incompleto.

**Corrección aplicada**: `Crypto.cpp` ahora incluye `BellLogger.h` (no
`esp_log.h`) y llama `BELL_LOG(error, CRYPTO_TAG, "...", args...)` —
mismo patrón que `EncodedAudioStream.cpp`, con `CRYPTO_TAG = "crypto"`
como el tag propio de este archivo. `BELL_LOG` en sí quedó exactamente
como estaba (revertido tras el intento fallido).

**Verificado**: `idf.py build` limpio, 0 errores nuevos, mismo margen
de flash.

**Plan de acción para el acoplamiento restante (no aplicado, propuesto)**:
después de sacar `esp_log.h`, `Crypto.cpp` todavía depende de
`<mbedtls/bignum.h>` (`dhInit()`/`dhCalculateShared()`, el intercambio
Diffie-Hellman del pairing ZeroConf) — el comentario ya existente en
el código señala que ese header puede no ser público en un mbedTLS 4.0
"de fábrica" (no el fork/port que trae ESP-IDF). No es un bug: compila
y funciona perfecto para el único target real de este proyecto
(ESP32). Es una limitación de portabilidad latente, relevante solo si
algún día se compila `cspot`/`bell` nativo en host (por ejemplo, para
testear con ThreadSanitizer los fixes de concurrencia de F93,
discutido como opción pero no encarado todavía). Tres caminos posibles,
en orden creciente de esfuerzo:
1. **No hacer nada** — aceptar que este archivo puntual solo compila
   contra el mbedTLS de ESP-IDF. Es lo más simple y consistente con que
   el proyecto nunca tuvo como objetivo correr fuera de ESP32.
2. **Envolver el uso de `mbedtls_mpi_*` en un `#ifdef ESP_PLATFORM`**,
   con una implementación alternativa (a elegir: mbedTLS "de fábrica"
   con headers públicos si el host lo tiene instalado, o una librería
   bignum liviana vendorizada) para el `#else`. Sigue el mismo patrón
   que ya usa el resto de `bell` (`bell/CMakeLists.txt` ya tiene una
   rama `if(ESP_PLATFORM)`/`else` completa). Esfuerzo medio.
3. ~~**Reemplazar el DH manual por la API PSA de key-agreement**~~ —
   **investigado y descartado.** Se revisaron los headers de PSA reales
   que usa este proyecto
   (`tf-psa-crypto/include/psa/crypto_values.h`):
   `PSA_DH_FAMILY_RFC7919` es la **única** familia FFDH que define PSA
   en esta versión de mbedTLS — los grupos modernos de RFC 7919
   (TLS 1.3-era: 2048/3072/4096/6144/8192 bits). El primo que usa
   Spotify para ZeroConf es el "Well-known Group 1, 768-bit" de
   RFC 2409 (Oakley/IKE clásico, ver el comentario en `Crypto.h`) — no
   es uno de los grupos RFC 7919 (el más chico de esos es 2048 bits), y
   PSA no tiene ningún mecanismo para importar un DH con primo/
   generador custom (no existe nada tipo "domain parameters" en este
   SDK) — es una restricción de diseño de PSA, no una limitación de
   esta versión puntual: solo grupos con nombre estandarizados, sin
   forma de "traer tu propio primo" (mismo tipo de debilidad que
   explotó Logjam). La opción 1 (no hacer nada) queda como la
   recomendada dado esto — no hay margen real para migrar a PSA aunque
   se invirtiera el esfuerzo.

### F99 — Opción 2 de F98 aplicada: `mbedtls_mpi_*` (el DH de ZeroConf) ahora está explícitamente acotado a `#ifdef ESP_PLATFORM`, con un `#error` claro para builds no-ESP (Bajo, aplicado)

Continuación de F98: en vez de dejar el acoplamiento a
`mbedtls/bignum.h` implícito (el archivo simplemente no compila fuera
de ESP-IDF, con un error de header-no-encontrado poco claro), se lo
hizo explícito.

**Corrección aplicada** (`Crypto.cpp`): el `#include
<mbedtls/bignum.h>` y los cuerpos de `dhInit()`/`dhCalculateShared()`
quedan detrás de `#ifdef ESP_PLATFORM` / `#ifndef ESP_PLATFORM
#error ... #endif`. Si alguna vez se intenta un build nativo (host,
para lo que se charló de testear F93 con ThreadSanitizer), falla acá
con un mensaje que apunta a F98/F99 y explica por qué, en vez de un
`fatal error: mbedtls/bignum.h: No such file or directory` sin
contexto. **Deliberadamente no se implementó** una alternativa de
bignum para host (ni con una librería como GMP ni vendorizando una
propia) — sería trabajo especulativo para un build que hoy no existe
ni está planeado en firme; si en algún momento se retoma la idea de
testear con TSan, ahí se decide con el objetivo real enfrente.

**Bug encontrado y corregido en el camino (antes de llegar al build
limpio)**: el primer intento reordenó el include de
`<mbedtls/bignum.h>` para que quedara después de `<psa/crypto.h>`, y
el build explotó con `mbedtls_mpi_init`/etc "not declared in this
scope" — a pesar de que el header existe y `ESP_PLATFORM` sí estaba
definido. Causa: el `mbedtls/bignum.h` que expone ESP-IDF
(`components/mbedtls/port/include/mbedtls/bignum.h`) hace `#define
MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` y recién ahí un
`#include_next` del header real (privado por defecto en mbedTLS 4.0,
ver F15) — pero `psa/crypto.h`, incluido primero en el intento fallido,
ya arrastraba transitivamente ese mismo header interno **sin** el
define puesto; su propio `#pragma once` lo dejó bloqueado para
siempre, y el `#include` de bignum.h de más abajo se volvió un no-op
silencioso. Solución: `<mbedtls/bignum.h>` tiene que quedar **antes**
de `<psa/crypto.h>` en el archivo — se restauró ese orden (era el
orden original, sin querer se había invertido) y se dejó un comentario
explícito para que nadie lo vuelva a romper por accidente.

**Verificado**: `idf.py build` limpio, 0 errores, 0 warnings nuevos,
mismo margen de flash.

### F100 — Rama no-ESP de F99 rellenada: exponenciación modular propia y autocontenida, en vez de un `#error` (Bajo, aplicado)

F99 dejó la rama `#else` (no-ESP) como un `#error` explícito — honesto,
pero no funcional. El usuario pidió una implementación "dummy" que
realmente funcione. Se evaluó y se descartó reinstalar la dependencia
de mbedTLS del lado host (F98/F99 ya habían mostrado que el header
`bignum.h` "privado" ni siquiera está en la ruta de include pública
normal de un mbedTLS 4.0 de fábrica) — en cambio, se escribió una
exponenciación modular propia y mínima (`BigUint`, namespace anónimo en
`Crypto.cpp`), sin ninguna dependencia externa nueva (nada de GMP/
OpenSSL — más portable en el sentido literal: compila en cualquier
host con un compilador C++17, sin instalar nada).

**Respaldo adicional para esta decisión**: el usuario aportó un hilo de
la lista de correo oficial de mbedTLS
([mbed-tls@lists.trustedfirmware.org](https://lists.trustedfirmware.org/archives/list/mbed-tls@lists.trustedfirmware.org/message/MGDE6KIQ7MCOIMFRKDMZ7UD2EHUCO4N3/))
donde Gilles Peskine (desarrollador central de mbedTLS) responde sobre
`MBEDTLS_ALLOW_PRIVATE_ACCESS` (el macro hermano de
`MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS`, mismo mecanismo): da acceso a
todos los campos/funciones privados, pero es **"un detalle de
implementación interno" que "puede cambiar en cualquier momento sin
aviso"** — y recomienda no depender de él salvo como último recurso,
documentado con un issue de GitHub de por medio. Confirma, desde la
fuente, que aunque hubiéramos logrado que el header compilara en un
host, hubiera sido apoyarse en algo que el propio proyecto mbedTLS no
garantiza mantener estable — otra razón más para la implementación
propia en vez de perseguir el acceso "privado" más allá de F99.

**Por qué esto no es "reinventar criptografía" en el sentido riesgoso
del término**: exponenciación modular (`base^exp mod m`) es un
algoritmo clásico y bien entendido (cuadrado-y-multiplicación en
binario), no un primitivo de seguridad novedoso — el riesgo real de
"no hagas tu propia criptografía" es sobre cosas como cifradores,
funciones de hash, o resistencia a ataques de canal lateral (timing),
no sobre el algoritmo de exponenciación en sí. Además, esta
implementación **nunca corre en el firmware real** — el ESP32 sigue
usando `mbedtls_mpi_*` sin cambios (rama `#ifdef ESP_PLATFORM`); esto
solo se ejecutaría en un build de host hipotético, para testing, con
un modelo de amenaza mucho más acotado (intercambio de pairing local,
no la sesión TLS principal).

**Verificación real, no solo "se escribió y se asumió que anda"**:
1. Se probó el algoritmo aislado contra `pow(base, exp, mod)` de
   Python (aritmética de precisión arbitraria nativa, la referencia
   más simple de confiar) usando el primo real de 768 bits de este
   proyecto, el generador real (`2`) y exponentes aleatorios — 6/6
   casos coincidieron exactamente.
2. Se extrajo el código **real** ya pegado en `Crypto.cpp`
   (`BigUint`, y los cuerpos de `dhInit()`/`dhCalculateShared()`, no
   una reescritura aparte) y se compiló standalone con `g++` (fuera de
   ESP-IDF), simulando un intercambio Diffie-Hellman completo entre
   dos partes independientes ("dispositivo" y "app") — ambas llegan al
   mismo secreto compartido byte por byte. Esto ejercita el código tal
   cual quedaría compilado en un build de host real, no una versión
   aparte "parecida".

**Corrección aplicada** (`Crypto.cpp`): clase `BigUint` (namespace
anónimo) con las operaciones mínimas necesarias — `fromBytes`/
`toBytes` (conversión con el mismo formato big-endian que
`mbedtls_mpi_read_binary`/`write_binary`), `multiply`, `mod`
(reducción por resta-y-corrimiento bit a bit), y `modExp`
(cuadrado-y-multiplicación). `dhInit()`/`dhCalculateShared()` ahora
tienen una rama `#else` real con la misma matemática que la rama
`#ifdef ESP_PLATFORM`, usando `BigUint` en vez de `mbedtls_mpi_*`.
Deliberadamente **no** está optimizada ni protegida contra ataques de
timing (comentario explícito en el código) — no hace falta para un
path que nunca corre en producción.

**Verificado**: `idf.py build` limpio para ESP32 (la rama `BigUint`
completa queda excluida de esa compilación, detrás de `#ifndef
ESP_PLATFORM` — no afecta el firmware real en absoluto), 0 errores, 0
warnings nuevos, mismo margen de flash. La rama no-ESP se verificó por
fuera del build de ESP-IDF, con `g++` directo, según el punto 2 de
arriba — sigue sin poder probarse dentro de un build real de `bell`/
`cspot` en host, porque ese build en sí nunca se armó (fuera de
alcance de este cambio puntual).

### F103 — `LoginBlob::buildZeroconfInfo()` mandaba `"deviceType": "deviceType"` literal en vez de `"SPEAKER"` - ícono equivocado en el picker de la app (Bajo, aplicado)

Reportado por el usuario: en el selector de dispositivos de la app,
otros clientes en su red (`go-librespot`, un receptor Sangean) aparecen
con el ícono de parlante, pero `cspot` aparece con otro ícono distinto.

**Causa**: `LoginBlob.cpp:230` (`buildZeroconfInfo()`, la respuesta que
sirve el endpoint HTTP de Zeroconf que la app consulta al descubrir el
dispositivo en la red local - `cspot_connect.cpp:207`,
`handleGet()`) tiene dos ramas, cJSON y nlohmann. Este proyecto
compila con `BELL_ONLY_CJSON` (`components/CMakeLists.txt (repo root)`), así
que la rama que realmente corre es la de cJSON - y ahí:

```cpp
cJSON_AddStringToObject(json_obj, "deviceType", "deviceType");
```

manda el string literal `"deviceType"` como valor del campo, en vez de
un tipo real (`"SPEAKER"`, como sí hace correctamente la rama nlohmann,
muerta en este build, dos líneas más abajo en el `#else`). La app no
reconoce `"deviceType"` como ningún tipo válido y cae a un ícono
genérico en vez del de parlante. Nada que ver con
`connectstate_DeviceType_SPEAKER` (`PlayerEngine.cpp`,
`buildDeviceInfo()`) - ese es un campo distinto, del connect-state PUT
vía Dealer/spclient, y ya estaba bien seteado desde antes; este bug es
específico del blob de Zeroconf, la capa de descubrimiento/pairing en
la red local.

**Corrección aplicada**: `"deviceType"` ahora manda `"SPEAKER"` en la
rama cJSON, igual que la rama nlohmann.

**Verificado**: build de host limpio, suite `unit_tests` 27/27, 112/112
asserts sin regresión (`LoginBlob.cpp` es parte de esa suite). Ningún
test cubre el contenido de `buildZeroconfInfo()` puntualmente. Pendiente
confirmar en hardware/app real que el ícono cambia a parlante.

### F104 — Barra de progreso del cliente ~2s adelantada del audio real, se "corrige" al pausar - sesgo sistemático en `TimeProvider::syncWithPingPacket()` (Bajo/cosmético, mitigación parcial aplicada)

Reportado por el usuario tras una sesión de prueba real: la barra de
progreso en el cliente de Spotify corre ~2 segundos adelantada respecto
al audio que efectivamente se escucha, constante (no sigue creciendo) y
desaparece la percepción del desfase al pausar.

**Causa**: `TimeProvider::syncWithPingPacket()` (`TimeProvider.cpp:14`)
calcula `timestampDiff = remoteTimestamp - getCurrentTimestamp()` con dos
sesgos sistemáticos, ambos en la misma dirección:

1. El timestamp que manda el AP de Spotify en el ping viene en **segundos
   enteros** (`*1000`, sin fracción) - trunca hacia abajo el instante real
   en que se generó, hasta 999ms (promedio 500ms).
2. `getCurrentTimestamp()` se evalúa cuando **recibimos** el ping, no
   cuando el servidor lo generó - el tránsito de red (unidireccional, no
   medido) se suma al sesgo, en la misma dirección.

Con álgebra simple (ver derivación completa en la conversación): el
resultado es que `getSyncedTimestamp()` queda sistemáticamente **atrasado**
respecto al tiempo real, por `(tránsito_de_red + cuantización)`. El
cliente real de Spotify extrapola la barra como `posición_guardada +
(su_reloj_ahora - nuestro_timestamp_reportado)` - un timestamp atrasado
infla esa resta, así que la barra corre adelantada exactamente por ese
sesgo. Al pausar, `playback_speed=0` - el cliente deja de extrapolar y
muestra el número tal cual, sin restar contra su propio reloj - el sesgo
deja de ser visible (no se corrige el valor, deja de haber operación que
lo exponga). Cosmético: no afecta el audio en sí, mismo espíritu que F97
(que era un bug distinto, en el `SpircHandler` ya eliminado - Fase 6 de
`docs/dealer_websocket_migration.md`).

**Mitigación aplicada**: `remoteTimestamp += 500` antes de calcular
`timestampDiff` - centra el sesgo esperado de la cuantización en 0 en vez
de un `-500ms` garantizado. El tránsito de red (el otro sumando) **no se
corrige** - no hay forma de medirlo con este keepalive unidireccional
(sin eco de ida y vuelta propio); típicamente más chico que el término de
cuantización, pero no cero. Se decidió no adivinar un valor fijo para
compensarlo sin base real para validarlo.

**Alternativa considerada, no aplicada**: sincronizar contra NTP real
(`esp_sntp`) en vez de piggybackear en el ping del AP - eliminaría ambas
fuentes de sesgo, pero es un cambio de alcance mayor (nuevo componente,
inicialización en el arranque de Wi-Fi, nueva dependencia de red) fuera
de lo que se pidió corregir acá.

**Verificado**: build de host limpio, `unit_tests` 27/27, 112/112 asserts,
`f93_concurrency_test`/`f87_logger_concurrency_test` sin regresión.
Ningún test cubre el valor numérico de `timestampDiff`. **Pendiente
confirmar en hardware real** que el desfase se redujo (no se espera que
desaparezca del todo - el término de tránsito de red queda sin corregir).

### F105 — Cliente muestra el dispositivo como "desconectado" tras un transfer en pausa: la app (`cspot_connect.cpp`) mandaba su propio PUT con estado obsoleto, en carrera con el del motor (Alto, encontrado en hardware real, corregido de raíz - pendiente confirmar en hardware)

Reportado por el usuario más de una vez, en distintas sesiones de
refactoring de `PlayerEngine`/`librespot-cpp`, siempre con el mismo
patrón difícil de fijar: el cliente móvil transfiere la reproducción al
ESP32 con el track **pausado** (el usuario no llegó a apretar resume), y
en algún momento posterior la app muestra el dispositivo como
desconectado - aunque el WebSocket del dealer sigue sano (pings/pongs) y
el resto de los PUTs reportan `is_active=1` correctamente. Apretar resume
en la pantalla local del equipo (que dispara un PUT real con
`is_paused=0`) lo arregla al toque. No había forma confiable de
reproducirlo a pedido - dependía de la ventana de tiempo entre el
`transfer` y que el primer chunk de audio realmente llegara a sonar, algo
que varía con la latencia de red de cada corrida.

Esto llevó a más de una sospecha equivocada a lo largo de varias sesiones:
primero se lo tomó por comportamiento esperado del cliente de Spotify
(rechazado por el usuario: "es un bug, si antes no sucedía"); después,
sin poder bisectar por falta de reproducción confiable, se sospechó del
commit del fix de flicker (`8b45f4d`, que agregó el primer PUT temprano
desde `trackLoadedCallback`) como causante más probable, sin confirmar.

**Causa real, confirmada con un log de hardware + trazado de código**: no
tiene nada que ver con `8b45f4d` ni con el motor de `librespot-cpp` -
está en la capa de la app, en el handler de `EventType::PLAY_PAUSE` de
`components/cspot/cspot_connect.cpp`. Ese handler llama
`dealer->updatePlayerState(..., currentTrackUri, ...)` usando una
variable de la app (`currentTrackUri`) que solo se llena en el handler de
`EventType::TRACK_INFO`. El problema: `PLAY_PAUSE` se dispara desde
`trackLoadedCallback` (fijado en el constructor de `PlayerEngine` /
`PlaybackController`) apenas se carga el track, mientras que
`TRACK_INFO` recién se dispara mucho después, desde
`reachedPlaybackCallback`, una vez que el audio empieza a producir
frames de verdad - en la corrida que expuso el bug, con 10-20s de
diferencia por un CDN lento. Para el primer track de la sesión,
`currentTrackUri` todavía está vacío en ese momento: el `PLAY_PAUSE`
manda un PUT con `has_track=0`/`is_playing=0` que pisa el PUT correcto
que `trackLoadedCallback` acababa de mandar un instante antes. Ese PUT
vacío es justo lo que el cliente interpreta como "se desconectó".

La causa exacta del `pos_as_of_ts` en el PUT espurio (idéntico al de la
posición real del track, no `0`) fue lo que permitió descartar
`notifyAudioEnded()` como origen (esa función siempre manda posición
`0`) y encontrar el verdadero call site.

**Por qué costó tanto encontrarlo**: el bug vive en la intersección de
dos módulos que cambiaron varias veces en paralelo durante el
refactoring SRP (`PlayerEngine`/`PlaybackController` en
`librespot-cpp`, y el event handler en `cspot_connect.cpp` del lado de
la app) - cada sesión que lo disparaba coincidía con cambios recientes en
uno de los dos módulos, lo que alimentó sospechas sobre el módulo
equivocado repetidas veces. La causa real nunca tocó ninguno de los
archivos que se venían refactorizando.

**Primera corrección aplicada (parche puntual, superada)**: en
`cspot_connect.cpp`, guardar el `PLAY_PAUSE` handler con
`!currentTrackUri.empty()`. Suficiente para el caso observado, pero
insuficiente como arreglo: dejaba intacta la causa de fondo - que
`cspot_connect.cpp`, una capa de adaptación específica de la placa (I2S,
mDNS, servidor HTTP de ZeroConf), tenía la capacidad de reconstruir y
mandar un PUT de connect-state a partir de su propio caché, en paralelo
con el motor. El usuario lo objetó explícitamente: "no es razonable que
código allí presente afecte la estabilidad de toda la librería" - un
parche que solo tapa el síntoma puntual deja abierta la puerta a que
reaparezca en otra forma (ya había pasado más de una vez, con distintas
sospechas equivocadas cada vez, ver arriba).

**Corrección de fondo aplicada**: se movió la responsabilidad de mandar
el PUT enteramente adentro de `PlayerEngine` (`librespot-cpp`), a
los call sites que ya tienen el estado real en el momento exacto en que
ocurre:
- El lambda `onTrackReached` (constructor, dispara `TRACK_INFO`) ahora
  también manda su propio `updatePlayerState()`, con
  `track->ref.uri`/`track->trackInfo.duration`/
  `playbackController.isPlaying()`/`getPositionMs()` - datos que ya tenía
  a mano, en vez de depender de que la app reconstruyera lo mismo desde
  un caché aparte.
- `PlayerEngine::setPause()` (el único punto de entrada real para
  pausa/resume, tanto remoto vía `player/command` como local vía botón
  físico) ahora manda su propio `updatePlayerState()` directamente,
  igual que ya hacían `seekMs()`/`handleSetVolume()`.
- `updatePlayerState()` pasó a **privado** en `PlayerEngine`, y el
  passthrough `DealerClient::updatePlayerState()` que le daba acceso a
  código externo **se eliminó por completo** (no tenía otro consumidor -
  era un remanente de la época de `SpircHandler`, ya migrada). No queda
  ningún camino para que `cspot_connect.cpp` (ni ningún otro código fuera
  de `PlayerEngine`) vuelva a mandar un PUT con estado propio -
  cerrado en el compilador, no solo en este caso puntual.
- En `cspot_connect.cpp`: se borraron las dos llamadas a
  `dealer->updatePlayerState()` (`PLAY_PAUSE`/`TRACK_INFO`) y los
  miembros `currentTrackUri`/`currentTrackDurationMs`, que quedaron
  completamente muertos - junto con los `#include` de `TrackReference.h`/
  `Utils.h` que solo existían para reconstruir el URI ahí.

**Verificado**: build de host limpio
(`FETCHCONTENT_SOURCE_DIR_LIBRESPOT_CPP` apuntando al clon local),
`unit_tests` 45/45, `engine_test` 17/17, `f93_concurrency_test`/
`f87_logger_concurrency_test` sin regresión. El cambio en
`cspot_connect.cpp` en sí **no se puede cubrir con la suite de host** -
es código de la app ESP32 (usa `notify()`, tipos de IDF), no forma parte
de `components/cspot/tests`. **Pendiente confirmar en hardware real**
repitiendo el mismo escenario (transfer con track pausado) y verificando
que ya no aparece el PUT con `has_track=0` antes de que el audio
arranque.

Commiteado en `librespot-cpp` (`91de497`, rama `master`).
