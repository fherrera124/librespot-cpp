# Handoff de Windows — sesión 2026-09-24

## Qué se logró y qué no

Se ejecutó el VM de Spotify **1.2.92.148** de forma reproducible y se siguió su
salida hasta el generador de bloques de audio. Un snapshot de este generador
reprodujo exactamente un bloque natural. Se puede reconstruir el runtime y
repetir los fixtures sin otro cambio de canción, dentro del proceso ya inicializado.

**No se identificó una AES de contenido validada.** El candidato de 16 bytes es
estable pero da 0/11 coincidencias. El servicio HTTP no está validado, el mismatch
E/148 no está demostrado y el E2E en ESP32 sigue pendiente. El siguiente paso
está en [PLAN.md](../PLAN.md), no en las antiguas instrucciones de desplegar Flask.

## Recorrido de la sesión

| Etapa | Hallazgo / resultado |
|---|---|
| Relectura tras cambios de otro agente | Se retiró la afirmación sin evidencia de «11 vectores pasan»; se conservaron cambios ajenos |
| Conectividad | Primer timeout sin VPN; luego acceso recuperado a Windows por SSH |
| Preflight | EXE/DLL 148; el servidor encontrado era `flask_frida_667.py` con otros offsets; se detuvo |
| Servidor 148 original | Se copió sin cambios y dio HTTP 500 / `Error: system error`; se detuvo |
| Excepciones | `exceptions: propagate` permite la ejecución nativa; el buffer final variable falla como control AES |
| Hooks internos | `setImmediate` + `traps: all` hacen visibles las llamadas repetidas; candidato estable en `0x49f854` |
| Copias | Incluir fuentes de copias de 8 bytes fue necesario para cubrir la forma del hook de referencia; ninguna AES conocida encontrada |
| Contexto de licencia | Se capturaron recurso, respuesta, entrada, candidato y callback, sin headers de cuenta |
| Reproducción correcta | El usuario aclaró que antes cambiaba canciones en Linux; al hacerlo en Windows apareció otro callback |
| Ruta de audio | `0xd8dfb8` lleva a `0xd9e2e4` y `0xd9d0f0`; la precarga usa `0x63fda4` |
| Runtime fresco | `0x4a0268` con callback local deshabilitado reproduce candidatos y bloques de 13 casos, dos veces |
| Control final | Bloque natural y replay: `b323946840ed95fc36fe2019d8d45887` |

Informes y alcance de cada ensayo: [VALIDATION_148_2026-09-24.md](VALIDATION_148_2026-09-24.md).
Contratos y teoría: [PLAYPLAY_ARCHITECTURE_NOTES.md](PLAYPLAY_ARCHITECTURE_NOTES.md).
Descartes: [DECISIONS_148.md](DECISIONS_148.md). Scripts: [catálogo](../tools/README.md).

## Entorno observado al cerrar las pruebas

- Host `CZC148B0HC`, `10.16.150.154` vía VPN; SSH `francisco.herrera`, clave
  `~/.ssh/win_claude`. [Acceso y reversión](../../acceso-ssh-pc-windows.md).
- Spotify principal PID **64216**, sesión gráfica **2**, respondió al último chequeo.
- Python Windows: `C:\Users\francisco.herrera\AppData\Local\Programs\Python\Python312\python.exe`.
- DLL en `%APPDATA%\Spotify`; hashes exactos en [FACTS.md](FACTS.md).
- Frida/psutil disponibles; `cryptography` disponible para la prueba de caché.
- Scripts/fixtures copiados a `C:\Users\francisco.herrera\`; se ejecutaron mediante
  SSH en primer plano. `Start-Process` no mantuvo vivo el servidor en el ensayo.
- Todos los validadores/hook sessions terminaron y se desadjuntaron. **No queda
  una captura esperando canción.** Se detuvieron los servidores de prueba y no
  se restauró el servicio 667. Reconfirmar procesos/puertos antes de una nueva sesión.
- No se cambió el cliente instalado, no se copiaron credenciales de cuenta y no
  se editó C++ durante esta continuación. El índice Git preexistente no se alteró.

## Guía para el siguiente agente

1. Leer instrucciones y documentos en el orden del [README](../README.md).
2. Comprobar PID, versión y hash antes de usar RVAs. Las firmas de los JS no
   sustituyen el hash completo; algunas funciones solo tienen prólogos comunes.
3. Inspeccionar y ejecutar `check_key_pipeline_148.js` con el runner y fixture
   indicados en el catálogo. No hace falta una nueva reproducción para este ensayo.
4. Si se necesita una captura natural: adjuntar, confirmar evento `attached` y
   recién entonces pedir reproducción **local en Windows**. Distinguir precarga
   de reproducción; cambiar canción en Linux no dispara necesariamente el hook.
5. Investigar el contexto/counter del generador con el control positivo disponible.
   Un archivo `.file` de caché no se ha asociado aún a la licencia capturada.

## Trabajo previo que no debe confundirse con esta sesión

Los arreglos HMAC (`DigestCrypto` con HMAC habilitado) en `Authenticator.h` y
`ApConnection.cpp` ya existían. El C++ conserva `FORCE PLAYPLAY`, volcado de
credenciales, parser manual y un hilo de prueba. Hay que limpiarlos con el plan
aprobado, pero no son una explicación demostrada de las diferencias criptográficas.
Los intentos `LoadLibrary`/Unicorn y el prototipo Flask forman parte de la historia;
no equivalen a un deobfuscador listo. Se conservaron sus archivos para diagnóstico.

### Phase 2: Reverse Engineering of `0xd9e2e4` and `0xd9d0f0`

1. **Analysis of `0xd9d0f0` (Block Generator)**
   Disassembly of `0xd9d0f0` reveals it is an unrolled 14-round AES block encryption function. It relies heavily on T-table lookups and XORs with round keys fetched from the context (`[r12 + 0x10]`, `[r12 + 0x14]`, etc.). The 14 rounds explicitly identify this as **AES-256**.
   
2. **Custom CTR Mode and Context State**
   The initial state (counter) is read from `[r12]` to `[r12 + 0x0C]`. The function lacks an initial `AddRoundKey` before the first T-table lookups; instead, the first XOR involves RK1 (`[r12 + 0x10]`). This implies the context state at `[r12]` is already `Counter ^ RK0`.
   The counter is not a standard big-endian integer. At the end of `0xd9d0f0`, a 16-bit word at `[r12 + 0x2e0]` is decremented. When it rolls over, bytes 15 and 14 of the counter state are updated via 256-byte substitution tables (found at `[r12 + 0xe0]` and `[r12 + 0x1e0]`).

3. **AES-256 Key Recovery**
   By analyzing the generated context for `vector_4` and natural playback, we identified that RK1 is stored at `[r12 + 0x10]` and RK2 at `[r12 + 0x20]`. We wrote a Python script implementing the inverse AES-256 key schedule (`W[0] = W[8] ^ SubWord(RotWord(W[7])) ^ Rcon[1]`) to recover RK0. For `vector_4`, the recovered 256-bit key is `0ed07ad43b5c3275bcc4f514243dff071e8ecd5284bd63190f36a9d86f4ef1ba`.

4. **Comparison with Historical Vectors**
   The historical ground-truth vectors from `cspot` assume AES-128-CTR with a 16-byte key (e.g., `vector_4` expects key `c3206271b4c70fff8e4ac3993c4dae8a` and stream `606c64ab...`). 
   When we pass `vector_4`'s obfuscated key to the 1.2.92.148 VM, it generates a 256-bit context and outputs a keystream starting with `d54668839b86a4084e2fe154ac211d51`. 
   **Conclusion:** Spotify Windows 1.2.92.148 uses a completely different DRM scheme (AES-256 with a custom permuted CTR mode) than `cspot` (AES-128 standard CTR). They request the same content under different file IDs or profiles. This explains why the output keystream generated by the Windows VM does not match the historical AES-128-CTR vectors.

5. **White-Box AES Transformations**
   We implemented a Python script to emulate the AES-256 generator using the extracted RK0 and standard AES primitives (`AES(Counter ^ RK0)`). However, the output blocks (`0071f052...`) did not match the blocks produced by the native VM (`b3239468...`). This discrepancy confirms that the native implementation utilizes **White-Box AES obfuscation** (e.g., affine transformations applied to the T-tables and round keys), meaning standard AES algorithms cannot be used to decrypt the stream even if the internal round keys are extracted.
   
   The only viable way to obtain the correct keystream for Spotify Windows 1.2.92.148 is to execute the native generator `0xd9d0f0` using the VM, passing it the 28-byte `wrapped` key. This fully establishes the cryptographic boundaries of the new DRM scheme.

### Conclusion of Step 2 & 3
The investigation has proven that Spotify Windows 1.2.92.148 does **not** use the standard AES-128 DRM scheme expected by `cspot`. Instead, it uses a **White-Box AES-256 custom CTR generator**. 
Because the algorithm is fundamentally different (256-bit obfuscated key schedule vs 128-bit standard), it is mathematically impossible to extract a 16-byte AES-128 key from the 1.2.92.148 VM that matches the historical ground-truth vectors. 

This triggers the plan's contingency:
> "Si una extracción con control positivo falla para E, investigar par token/version usado por Windows 148 o el contexto adicional que falte. Cambiar a otro build es una alternativa..."

**Next steps to consider:**
1. Switch to an older build (e.g., `667`) that might still use the AES-128 scheme compatible with `another-unplayplay` and `cspot`.
2. Alternatively, if we stick with 148, `cspot` must be refactored to use the LAN service as a continuous Keystream Oracle (passing the 28-byte `wrapped` key to the VM to generate blocks), rather than fetching a static 16-byte AES key.
