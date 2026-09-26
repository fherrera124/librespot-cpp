# Hechos, parámetros y procedencia

Síntesis actual: [STATUS](../STATUS.md). Los ensayos históricos con entradas E
sobre el build 148 son evidencia de ABI y de resultados negativos acotados;
esas entradas no son licencias token148 validadas por contenido.

**2026-09-25:** [API HTTP AES16](HTTP_DFA_SERVICE.md) verificada con dos licencias
guardadas y prefijos CRC válidos. El cliente C++ envía ambos campos de licencia,
valida JSON/AES y admite URL/token. CLI compilada; reproducción completa pendiente.

Corte de capturas **2026-09-24**. [Arquitectura](PLAYPLAY_ARCHITECTURE_NOTES.md)
desarrolla los contratos; [validación histórica](VALIDATION_148_2026-09-24.md)
y [manifiesto](../runs/20260924-discovery-148/manifest.json) enlazan la evidencia.

## Build realmente ejecutado

**Actualización posterior:** [token148/v5](TOKEN148_ONESHOT_2026-09-24.md),
`02d29f82a8396930aab0a5885c81da7a`, fue aceptado HTTP200 en dos recursos y su
generador 148 coincide con AES-128-CTR durante 4096 bytes por recurso. Contenido
Ogg/Vorbis y CRC válidos. PID reverificado 72132, mismo hash de disco. SpClient.cpp
ya contiene ese token por el cambio externo; las menciones a E debajo son
históricas. No se extrajo AES16 del candidato ni del descriptor.

- Windows EXE/DLL: **1.2.92.148**. Preflight anterior: PID64216; ensayo token148:
  PID72132. Ambos son datos históricos; volver a verificar.
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
| `0x49f627` | Llamada a `0x49f854` | Confirmada por desensamblado del dump 148; no es un hook de AES |
| `0x49f854` | Función que recibe el candidato de 16 bytes en RDX | Control natural positivo; candidato no validado como AES |
| `0x49f894` → `0x49f994` | Llamada a función que lee `gs:[0x58]` | Relación estática; no se demostró que exponga K0 |
| `0x49f8ea` | Copia de 3072 bytes desde RDI=`args[0]` mediante `0x17780e0` | Desensamblado del dump 148; no demuestra que sean tablas AES |
| `0x49f904` | Copia de ese candidato; retorno `0x49f909` | Trazado de copia, tamaño 16 |
| `0x49f925` | Otra copia de 16 bytes desde R12=`args[3]` | Desensamblado del dump 148; contenido/función del argumento no atribuidos |
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

Los 11 ensayos E anteriores se conservan en la [validación histórica](VALIDATION_148_2026-09-24.md)
como pruebas de repetibilidad y ABI del build 148. Sus pares AES procedían de
otra referencia y carecían de `b4_seq` y respuesta completa; no son ground truth
criptográfico del token148/v5. Los dos controles token148 actuales están en
[ground truth schema2](../data/ground-truth-vectors.json), con
[verificador offline](../tools/check_ground_truth_148.py), [hito DFA](../dfa_attack_results.md)
y [API HTTP](HTTP_DFA_SERVICE.md).

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
  AES-128-CTR. La API HTTP verificó dos licencias y la CLI compiló; reproducción completa de cspot sigue pendiente.

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

## Referencias metodológicas

- [Frida NativeFunction](https://frida.re/docs/javascript-api/#nativefunction): excepciones y traps.
- [SpotiLoad, commit inspeccionado](https://github.com/cycyrild/SpotiLoad/tree/6dfddf683b5b3b207550f86aecd955fb3bc36b64): layout de request y captura de AES para otro build.
  Checkout usado en `/tmp/playplay-spotiload-reference-20260924`, no persistencia garantizada.
- [another-unplayplay](https://github.com/cycyrild/another-unplayplay): referencias de emulación y vectores;
  es un modelo de método condicionado al hash y ABI del binario exacto. Sus RVAs no equivalen a los del build 148.

Los campos similares de dos configs no prueban que los harness sean intercambiables:
SEH, runtime, offsets de extracción y tamaños se verifican para cada binario.
La [búsqueda directa](DIRECT_AES_SEARCH_148.md) terminó negativa en las ventanas
examinadas; [Unicorn](UNICORN_FEASIBILITY_148.md) reproduce contextos guardados
pero no construye uno desde una licencia nueva.
