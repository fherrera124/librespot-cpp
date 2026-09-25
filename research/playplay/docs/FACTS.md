# Hechos, parámetros y procedencia

Síntesis actual: [STATUS](../STATUS.md). Las tablas de tokens A–F y preflights
anteriores conservan su fecha; no sustituyen el control token148.

**2026-09-25:** [API HTTP AES16](HTTP_DFA_SERVICE.md) verificada con dos licencias
guardadas y prefijos CRC válidos. El cliente C++ envía ambos campos de licencia,
valida JSON/AES y admite URL/token. CLI compilada; reproducción completa pendiente.

Corte **2026-09-24**. Distinguir resultados históricos del backend de lo medido
localmente en esta sesión. [Arquitectura](PLAYPLAY_ARCHITECTURE_NOTES.md) desarrolla
los contratos inferidos; [validación](VALIDATION_148_2026-09-24.md) enlaza la evidencia.

## Build realmente ejecutado

**Actualización posterior:** [token148/v5](TOKEN148_ONESHOT_2026-09-24.md),
`02d29f82a8396930aab0a5885c81da7a`, fue aceptado HTTP200 en dos recursos y su
generador 148 coincide con AES-128-CTR durante 4096 bytes por recurso. Contenido
Ogg/Vorbis y CRC válidos. PID reverificado 72132, mismo hash de disco. SpClient.cpp
ya contiene ese token por el cambio externo; las menciones a E debajo son
históricas. No se extrajo AES16 del candidato ni del descriptor.

- Windows EXE/DLL: **1.2.92.148**. Preflight anterior: PID64216; ensayo token148:
  PID72132. Ambos son datos históricos, sesión gráfica2; volver a verificar.
- DLL en disco: `C:\Users\francisco.herrera\AppData\Roaming\Spotify\Spotify.dll`.
- SHA256 en disco: `7b44456a90142daeb758e2736d8628ffe211d6ea523db8f1050b3c8b1addb68a`.
- Base viva observada: `0x7ff975d20000` (ASLR; no reutilizar como constante).
- Dump local crudo: `research/dlls/Spotify_1.2.92.148_dump.dll`, SHA256
  `275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc`.
- El dump usa **offset de archivo = RVA**. Un PE de instalador requiere mapeo
  de secciones. La inspección del header de este dump da ImageBase
  `0x7ff9b7da0000`; conserva referencias relocalizadas de su captura. No mapearlo
  a `0x180000000` sin resolver esas referencias. Tampoco reutilizar su base como
  base viva de otra sesión. [Ensayo Unicorn](UNICORN_FEASIBILITY_148.md).
- Preflight conservado en [windows-preflight-2026-09-24.json](windows-preflight-2026-09-24.json).

## Mapa de RVAs 148

Nombres descriptivos de investigación, no símbolos exportados del DLL.

| RVA | Papel observado | Evidencia / alcance |
|---|---|---|
| `0x49cb88` | Inicializador VM | Llamada estática en `0x4a0407`; ejecutada por la ruta de runtime fresco |
| `0x49eaa4` | `VmObjectTransform` | Hook natural y llamadas repetidas |
| `0x49f854` | Función que recibe el candidato de 16 bytes en RDX | Control natural positivo; candidato no validado como AES |
| `0x49f904` | Copia de ese candidato; retorno `0x49f909` | Trazado de copia, tamaño 16 |
| `0x49f961` | Copia final de 28 bytes | Instrucción `mov r8d, 0x1c` previa y trazado dinámico |
| `0x17780e0` | Rutina de copia | Firma única en dump y observación en vivo |
| `0x4a0494` | Manejador de respuesta PlayPlay | Recurso, protobuf y callbacks capturados |
| `0x4a0268` | Construcción del runtime + transformación | 13 casos × 2 sin nuevo evento natural |
| `0x4a041d` | Call directo a transformada; retorno `0x4a0422` | Xref y backtrace natural |
| `0x63fda4` → `0x640844` | Callback/ruta de precarga | Callback capturado; cadena posterior estática, receptor virtual sin captura confirmada |
| `0xd8dfb8` → `0xd90808` | Callback/ruta de reproducción | Callback capturado; cadena posterior estática |
| `0xd9b55c` → `0xd9ce94` → `0xd9d088` | Preparación del contexto de bloques | Desensamblado; `0xd9ce94` es un salto a `0xd9d088` |
| `0xd9e2e4` | Inicializador desde salida de 28 bytes | Llamadas nativas sobre buffers privados |
| `0xd9d0f0` | Generador de bloque de 16 bytes | Control natural positivo con snapshot `0x2e4` |

La procedencia inicial de `0x49cb88`/`0x49eaa4` no quedó guardada como buscador.
La sesión actual confirma su ejecución, no una derivación de cero por firma.
[Playbook](RVA_DISCOVERY_PLAYBOOK.md) contiene procedimientos y límites.

## Tamaños, constantes y fixtures

| Dato | Valor / límite |
|---|---|
| Entrada ofuscada | 16 bytes |
| Snapshot VM | 144 bytes; restaurar para cada llamada |
| Inicializador observado en args[3] | `a33c929c31c3e0eefeedebdaa1e7ff7f`, 16 bytes |
| Candidato previo | 16 bytes, RDX en `0x49f854` |
| Salida transformada 148 | 28 bytes; primeros 24 variables en repeticiones |
| Últimos cuatro bytes observados | `01000000` en capturas completas; significado no demostrado |
| Contexto copiado del generador | `0x2e4` = 740 bytes; no es el objeto VM de 144 bytes |
| Reserva usada por harness del generador | 4096 bytes, elección conservadora, no tamaño inferido de estructura |
| Auxiliar de inicialización del generador | 4 bytes, estable por entrada ensayada; significado pendiente |
| IV estándar usado por cspot/fixtures | `72e067fbddcbcf77ebe8bc643f630d93` |
| Ogg de los fixtures | `OggS` en offset 167; no confirmado para cualquier archivo de caché |

`data/ground-truth-vectors.json`: SHA256
`9c11d188e4443f4f42de8296230da5327eb329db68da151fc48a1b8dd91f9791`.
11 pares sobre 3 recursos: 3 v2, 3 v3, 3 v4 y **2 v5**. Además hay AES sin
entrada E y vectores publicados C/F. Los ensayos de esta sesión usan los 11 de E.
La procedencia de AES es la referencia publicada; las entradas E fueron obtenidas
en la investigación previa. El fixture no conserva `b4_seq` ni respuestas completas.

## Tokens PlayPlay (constante de 16 bytes en el request)

Las letras A–F son etiquetas de esta investigación; no una especificación de
Spotify. El significado del primer byte no está demostrado. Barrido histórico contra
`gew4-spclient.spotify.com` con la cuenta del usuario, `file_id` de control
`f5eb2b3e7a3798b4a55369bd3cc840fceddebb94`:

| Gen | Token (hex) | Origen | Request |
|---|---|---|---|
| A | `011bf34c8d0393bcda8b1d3eeaf8f3b2` | `re-unplayplay` (DMCA 2025-03-20) | 403 `0803` |
| B | `0132b6f3165865ff69a47d4321ff7520` | `@spdl/unplayplay` (npm) | 403 `0803` |
| C | `02811027c51620c0fd36cd1de59e227a` | `unplayplay` 0.0.9 (PyPI) / build 485 | 403 `0803` |
| D | `01e132cae527bd21620e822f58514932` | `uhwot/unplayplay` | 403 `0803` |
| **E** | **`01f62e56cd5435b90dde1a4fdf42af2d`** | **`emptygi/WaveeMusic`** | **200 (v2..v5)** ✅ |
| F | `027b23a2442c86ca4b004ddfef291954` | `another-unplayplay` / build 483 | 403 `0803` |

**Barrido token × version (E = control positivo):**

| token | v1 | v2 | v3 | v4 | v5 | v6 | v7 | v8 |
|---|---|---|---|---|---|---|---|---|
| **E** | 403 | **200** | **200** | **200** | **200** | 403 | 400 | 400 |
| A,B,C,D,F | 403 | 403 | 403 | 403 | 403 | 403 | 403 | 403 |

El barrido anterior usaba **E/v5**. SpClient.cpp ahora contiene **token148/v5**:
`02d29f82a8396930aab0a5885c81da7a`; HTTP200 y contenido comprobados en dos recursos
mediante la prueba puntual Linux/Windows, no E2E de cspot.


## Request/response e integración

- Endpoint ensayado: `POST https://{spclient}/playplay/v1/key/{file_id_hex}`.
- Request protobuf: `version`, `token` PlayPlay, interactividad, tipo, timestamp;
  headers de cuenta `Authorization` y `Client-Token`. El token PlayPlay estático
  **no** es una credencial de cuenta ni la clave ofuscada de respuesta.
- Respuesta según [schema](../../../protobuf/playplay.proto): campo 1
  `obfuscated_key`; campo 2 `b4_seq`. Las dos capturas completas de Windows
  tienen 16 y 4 bytes respectivamente. En esos ensayos antiguos no se capturó el
  request. La [nota externa](external/extraction_kPlayPlayToken_148.md) aporta
  token148/v5 y el control posterior valida su relación funcional con build148.
- El barrido histórico relacionó 403 con tokens rechazados; no prueba el orden
  de validaciones internas del backend. Una respuesta sin campo 1 no autoriza
  diagnosticar automáticamente «Widevine-only» sin analizarla.
- El request se documentó con credenciales web, login5 y device-flow. Esto no
  significa que toda credencial/scope futuro sea aceptado.
- cspot obtiene `file_id` mediante extended metadata TRACK_V4, resuelve CDN y usa
  AES-128-CTR. La prueba E2E PlayPlay no se hizo con una AES validada aquí.

Revisión del C++ existente (rutas desde la raíz del repositorio):

| Archivo / símbolo | Estado observado |
|---|---|
| `main/src/api/SpClient.cpp`, `playPlayLicense` | token148/v5, conserva obfuscated_key16 y b4_seq4; retirado volcado de credenciales |
| `main/src/FileProvider.cpp` | PlayPlay forzado, API JSON validada, URL local8765 por defecto; `PLAYPLAY_SERVICE_URL`/`PLAYPLAY_SERVICE_TOKEN` |
| `main/src/session/Session.cpp`, `connectDealer` | Hilo detached de prueba con track fijo |
| `main/include/proto/PlayPlayPb.h` / `protobuf/playplay.proto` | Schema y código nanopb presentes |
| `main/include/session/Authenticator.h`, `main/src/api/ApConnection.cpp` | Arreglos HMAC preexistentes; no modificados por esta sesión |

No describir `FileProvider` como un scaffold sin RPC: eso era un estado anterior.
El timestamp usa `std::chrono::system_clock`; el efecto de un reloj no sincronizado
es una hipótesis pendiente, no la causa demostrada del fallo criptográfico.

## Otros binarios de referencia

| Build | sha256 canónico (lo que espera el paquete) | token del par | ¿lo tenemos? |
|---|---|---|---|
| 1.2.88.483 | `9cafe1cad176024485f8840b72f6747d5b87885b0423b1df005adf088ef80ce8` | F | ❌ (solo sub-build) |
| 1.2.88.485 | `ed3b378d428c8b1034203d62676a0e77cdae157ef70acbbd30be1ba08b8fd045` | C | ❌ (solo sub-build) |
| 1.2.93.667 | (en repo privado de Wavee; no hay VAs públicas) | ? (¿E?) | ⚠️ sub-build extraída (ver abajo) |

**Binarios conservados** en `research/dlls/`. Las configuraciones canónicas
483/485 fallaron con estos hashes distintos; no son intercambiables por número
de versión. No se localizaron RVAs validados para nuestro 667 en esta sesión.

| archivo | versión | sha256 | ImageBase |
|---|---|---|---|
| `Spotify_1.2.88.483_g8aa8628e.dll` | 483 | `f88968879e3ce8be8ec87c45cc37843d8f175cfdd8b11fbe4e0e00f5b88b45f3` | `0x180000000` |
| `Spotify_1.2.88.485_g1012a6e0.dll` | 485 | `c2a0c44d087ba6de9cb6b0c6628eab3a22d45b1ed3d7c204ec5d9dbc52fbec71` | `0x180000000` |
| `Spotify_1.2.93.667_g7b5cc0ce.dll` | **667** | `3e2e6fa93a6fc2b2f23a5716d3b20274fcdf381dcb21c64147e0e092c9ee8bac` | `0x180000000` |
| `Spotify_1.2.92.148_dump.dll` | 148 | `275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc` | `0x7ff9b7da0000` (header del dump) |


El 667 proviene de un instalador full extraído con PE/overlay LZMA1. Los
hashes canónicos 483/485 son los esperados por sus paquetes, no los disponibles
localmente. No saltar el hash gate y dar por correctos los offsets.

## Fuentes de la investigación (versiones históricas)

- [Frida NativeFunction](https://frida.re/docs/javascript-api/#nativefunction): excepciones y traps.
- [SpotiLoad, commit inspeccionado](https://github.com/cycyrild/SpotiLoad/tree/6dfddf683b5b3b207550f86aecd955fb3bc36b64): layout de request y captura de AES para otro build.
  Checkout usado en `/tmp/playplay-spotiload-reference-20260924`, no persistencia garantizada.
- [another-unplayplay](https://github.com/cycyrild/another-unplayplay): referencias de emulación y vectores;
  copia Python consultada en `ppvenv/lib/python3.13/site-packages/unplayplay/`.
  Esa copia tiene el hash gate modificado (`if False`); no es una instalación limpia.
- [Origen histórico del token E](https://github.com/emptygi/WaveeMusic/blob/be546d5b14181e184a5b008f3a7f5bd7c64b746b/ConsoleApp1/Program.cs): fork público; no aporta un DLL validado de E.
- [WaveeMusic](https://github.com/christosk92/WaveeMusic): referencia histórica de enfoque nativo;
  derivación privada no disponible en esta investigación. El pin 667 citado en notas
  antiguas no se volvió a comprobar durante esta revisión documental.

Los campos similares de dos configs no prueban que los harness sean intercambiables:
SEH, runtime, offsets de extracción y tamaños se verifican para cada binario.
