# Referencia detallada de herramientas PlayPlay

Para elegir herramienta rápidamente: [CATALOG](../CATALOG.md) y
[STATUS](../STATUS.md). Este documento conserva métodos y herramientas del build 1.2.92.148.
Las cantidades de casos de los ensayos guardados describen aquellas corridas;
el ground truth activo reúne los dos controles token148/v5 validados.

**Servicio actual (2026-09-25):** [API HTTP de AES16](../docs/HTTP_DFA_SERVICE.md).
`playplay_dfa_service.py` + `playplay_dfa_rpc.js` usan Frida/DFA y exigen
`obfuscated_key`/`b4_seq`. `verify_playplay_service.py` verifica las respuestas
contra licencias y contenido desde Linux. Los prototipos de contrato parcial están archivados como fuentes del
experimento de descubrimiento148, no como servicio operativo.

- [Comprobar el workspace](check_workspace.py): catálogo, enlaces y hashes, offline.
- [Comprobar el cierre de búsqueda directa](check_direct_aes_148.py): anexo crudo y AES, offline.
- [Captura externa agrupada](token_capture_148/README.md): material histórico reubicado.

Corte **2026-09-24**. Leer el código antes de ejecutar, como exige
[AGENTS.md](../AGENTS.md). Los scripts 148 usan RVAs del binario exacto de
[FACTS.md](../docs/FACTS.md). Los controles históricos descritos aquí preceden
al caso DFA recuperado; ver [estado actual](../STATUS.md).
Los scripts de esta sesión están documentados individualmente abajo; las
herramientas anteriores se separan al final.

## Entornos y archivos

**Capturas recuperadas y protegidas:** [guía CLEAN_CAPTURE_148](../docs/CLEAN_CAPTURE_148.md).
`run_clean_capture.py` + `playplay_148_preflight.py` verifican versión/hash y
firmas antes de ejecutar `capture_clean_148.js` o `capture_context_148.js`.
Los originales con hash histórico están archivados en runs. Los cambios actuales
se comprueban offline con `test_clean_capture.py`; integración Windows pendiente.

**Control de contenido token148/v5:** estas herramientas validan el stream
nativo y contenido. La extracción AES16 se realiza por la vía DFA separada.
Comandos, fixtures e informes en [TOKEN148_ONESHOT](../docs/TOKEN148_ONESHOT_2026-09-24.md).

- `probe_token148_once.py`: Linux, requests/cryptography, login desde session.json,
  una licencia nueva y opcionalmente prefijo CDN; no persiste tokens de cuenta.
- `check_fresh_license_148.js`: Frida mediante el runner existente, b4_seq explícito,
  de 1 a 256 bloques por ejecución y comparación de dos ejecuciones.
- `verify_token148_content.py`: offline, cryptography; compara todos los bytes
  contra AES-128-CTR, descifra el prefijo y comprueba Vorbis/CRC Ogg. Exit0=éxito.

El runner archivado devolvía2 para eventos alternativos completos. Sus
informes deben leerse con el verificador independiente; no extrapolar ese
código de salida al runner actual.

| Entorno | Dependencias / uso |
|---|---|
| Linux, Python 3 | Biblioteca estándar para HTTP, sus tests y buscador de firma |
| Linux, `research/playplay/ppvenv/bin/python` | `pefile`, `capstone` para análisis estático; receta en el playbook |
| Windows, Python 3.12 | `frida`, `psutil` para runner; `cryptography` para caché; `flask` para el servicio actual (no necesario para revisión offline) |
| Scripts JS | Se cargan **con Frida**, no con Node ni directamente con Python |

Python Windows observado:
`C:\Users\francisco.herrera\AppData\Local\Programs\Python\Python312\python.exe`.
Los paquetes anteriores ya estaban disponibles; `Crypto` no estaba instalado.
No confundirlo con `cryptography`.

Para adjuntar, verificar de nuevo proceso principal, DLL y versión. El PID 64216
y sesión 2 son datos **históricos**, no constantes. PowerShell:

```powershell
Get-CimInstance Win32_Process -Filter "Name='Spotify.exe'" |
  Select-Object ProcessId, SessionId, CommandLine
(Get-Item "$env:APPDATA\Spotify\Spotify.dll").VersionInfo |
  Select-Object FileVersion, ProductVersion
Get-FileHash "$env:APPDATA\Spotify\Spotify.dll" -Algorithm SHA256
```

Elegir el proceso sin `--type=` ni crashpad, con sesión gráfica del usuario.
Verificar 1.2.92.148 y hash de disco documentado. No matar todos los Spotify para
«limpiar»: identificar y terminar solo el ensayo propio si quedó pendiente.

## Ground truth y runner de controles actuales

El [fixture schema2](../data/ground-truth-vectors.json) contiene exclusivamente
las dos licencias token148/v5 validadas. [Campos y separación de diagnósticos](../data/README.md).
El checker compara procedencia, hashes, K10→K0, bloque nativo y CRC:

```sh
python3 research/playplay/tools/check_ground_truth_148.py
python3 research/playplay/tools/test_ground_truth_148.py
```

`validate_windows_vm.py` es el runner para pruebas del build148. Las fuentes
exactas del control anterior permanecen en
[runs/20260924T164814Z-token148/source](../runs/20260924T164814Z-token148/source/)
y sus informes se evalúan con `verify_token148_content.py`; no se reemplazan
sus hashes por los de la revisión actual.

Los comandos siguientes de diagnósticos usan PowerShell, con `$ppPython` igual
al Python que tiene Frida/psutil y `$ppPid` igual al proceso principal recién
verificado. Definir `$ppRun = @("--pid", "$ppPid", "--vectors", "ground-truth-vectors.json", "--timeout", "180")`.
Copiar script, runner y `playplay_148_preflight.py`; para datos
específicos, copiar también `--script-data`. Los escritores requieren un nombre
de informe nuevo. Las AES de referencia no deben entrar en capturas destinadas
a buscar claves; los diagnósticos que inyectan referencias deben etiquetarse
como controles contaminados para ese propósito.

Para capturar los dos controles actuales, copiar además `check_fresh_license_148.js`:

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --capture-only --report control-nuevo.json
```

Evaluar el informe en Linux, con el repositorio y `cryptography` disponibles:

```sh
python3 research/playplay/tools/validate_windows_vm.py --evaluate-report /tmp/control-nuevo.json
```

La captura y un diagnóstico completado no equivalen a validación criptográfica.
El modo predeterminado no inyecta AES en Spotify; los modos custom suministran
referencias por compatibilidad con los trazadores y deben usarse como diagnósticos.
`--advance-track` necesita también `advance_spotify_track.js` junto al runner.

## Scripts JS de la sesión: función y ejecución individual

Todos los comandos siguientes usan `$ppPython`/`@ppRun` definidos arriba.
Copiar el JS y, si corresponde, su fixture. Cada comando propone un informe
nuevo; cambiarlo si ya existe. Para eventos naturales, esperar `attached` y
reproducir **en Windows**, no Linux ni otro dispositivo Connect. Caché/precarga
pueden evitar una nueva licencia aun cambiando de canción.

### extract_vm_key_148.js

Valida firma de `0x49f854`, captura el candidato RDX, VM de 144 bytes e
inicializador de 16; difiere replay hasta finalizar la llamada natural. Control
natural y once entradas dos veces, hooks por hilo, excepciones propagadas.
Emite `natural_output`, `control_replay`, once `result` y `done`. Resultado
conservado: control positivo, 11 pares estables, 0/11 AES; salida 1. La versión
actual guarda 28 bytes finales; el informe original guardó 24.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script extract_vm_key_148.js --report prewrap-nuevo.json
```

### trace_vm_copies_148.js

Traza copias de **exactamente 16 bytes** en `0x17780e0` dentro de la
transformada. Necesita una llamada natural; después ejecuta control y 11
vectores: 13 eventos `copies` y 13 `raw_output`. Máximo 2000 muestras por
llamada; conserva prefijo final de **24** bytes. Encontró call `0x49f904`.
La versión inicial tenía replays sin hooks; la actual difiere el trabajo y usa
`traps: all`. No confundir ambos informes.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script trace_vm_copies_148.js --report copies16-nuevo.json
```

### search_vm_aes_copies_148.js

Amplía a copias >=8 bytes y lee **16 bytes al inicio de cada fuente**, incluso
si la copia es de 8. Busca las tres AES conocidas; cuenta tamaños de todas las
copias, guarda hasta 2000 muestras por llamada y sigue buscando matches después
del límite. Necesita evento natural; 13 `copies` + 13 `raw_output`. Actual
lee 28 bytes finales; el informe histórico, 24. No es búsqueda exhaustiva de memoria.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script search_vm_aes_copies_148.js --report copies8-nuevo.json
```

### trace_key_consumers_148.js

Observa candidato, entrada y backtrace de una llamada natural y enumera exports
criptográficos (en el entorno observado estaba bcrypt). **No hookea las funciones
de descifrado ni invoca el VM.** La corrida guardada expiró sin llamada; no se
obtuvo una ruta AES mediante este script.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script trace_key_consumers_148.js --report consumers-nuevo.json
```

### capture_playplay_context_148.js

Hook a respuesta `0x4a0494`: captura recurso20, kind, callback, protobuf
(campos 1 y 2), input VM, candidato y salida28 de una misma llamada. Límite de
respuesta 256 bytes; no headers/token de request. Emite `playplay_context`
y `done`. La captura válida fue de precarga, no prueba asociación con un chunk.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script capture_playplay_context_148.js --report context-nuevo.json
```

### trace_playplay_consumer_148.js

Extiende el anterior: observa `0x640844` y adjunta temporalmente al receptor
virtual de precarga, registrando identificadores, descriptor28 y prefijo48 del
código del método receptor si se ejecuta esa ruta. Requiere **licencia de precarga** para observar
el receptor. La captura real siguiente usó callback de reproducción y produjo
solo `playplay_context` válido. Receptor virtual todavía pendiente.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script trace_playplay_consumer_148.js --report consumer-nuevo.json
```

### check_wrapped_keystream_148.js

Sin evento natural nuevo: inicializa contextos desde dos descriptores28
capturados, ejecuta un bloque dos veces y compara con AES(candidato, IV estándar).
Fixture: array con `file_id`, `candidate`, `wrapped`, `expected_stream`.
Usa `0xd9e2e4`/`0xd9d0f0`, reservas de contexto4096/salida32/auxiliar4.
Emite dos `keystream_control`; ambos estables, ningún match AES. La vigencia
de descriptores guardados después de reiniciar el cliente no fue validada.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script check_wrapped_keystream_148.js --script-data wrapped-key-controls-148-2026-09-24.json --report wrapped-nuevo.json
```

### check_key_pipeline_148.js

Es el ensayo de constructor/runtime nuevo usado para descubrir y contrastar el ABI148. El fixture es un array de 13 objetos:
`label`, `file_id`, `obfuscated`, `expected_candidate`, `aes`,
`aes_known`, `expected_stream`. Dos controles naturales + once E; en los dos
naturales `aes` es el **candidato hipotético**, y `aes_known=false`.
Los otros once usan la AES de referencia. No mezclar sus alcances.

Crea runtime nuevo con solicitud256 cero y byte+0x70=1; input16, auxiliar4 y
cuarto argumento NULL. Captura candidato y salida28 antes de terminar el caller,
luego inicializa contexto4096 y genera cuatro bloques. Dos ejecuciones por caso.
Emite 13 `pipeline_control`. `match` compara solo primer bloque contra la
hipótesis AES/IV; `deterministic` compara solo primeros bloques. **Comparar además
`blocks == repeat_blocks`** para comprobar los cuatro. Los informes conservan
la fase inicial de un bloque y la posterior de cuatro.

### capture_native_stream_148.js

Nuevo control natural durante reproducción local Windows: no exige licencia
nueva. Hook una vez en `0xd9d0f0`, copia contexto740 antes de la llamada y
salida16 después; desadjunta ese hook inmediatamente y difiere cuatro llamadas
sobre copia privada. Emite un `natural_stream_control` con `match=true` si
primer replay=natural. Guarda contexto completo en hex; no lo asocia a recurso.
El primer ensayo abortó por firma transcrita mal; el retry corregido pasó.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script capture_native_stream_148.js --report natural-stream-nuevo.json
```

### advance_spotify_track.js

Helper opcional: tras un segundo busca una ventana visible `Chrome_WidgetWin`
del PID y publica `WM_APPCOMMAND` Next Track (11) con `PostMessageW`.
Emite `playback_trigger`; mensaje aceptado no garantiza canción/licencia.
No tiene CLI independiente. Usar, por ejemplo:

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script capture_playplay_context_148.js --advance-track --report context-next-nuevo.json
```

Un ensayo automático puede terminar antes de que el helper se dispare. No usarlo
para el pipeline, que ya no necesita ese estímulo.

## Scripts Python de la sesión

### find_copy_anchor.py

Buscador offline de firma binaria con wildcards/DOTALL, sin argumentos ni paquetes
externos. Usa `research/dlls/Spotify_1.2.92.148_dump.dll` resuelto respecto del
script, layout raw. Imprime JSON con SHA256, patrón y RVAs. Código 0 exige una
coincidencia, 1 significa cero o varias. Resultado conservado: `0x17780e0`.

```sh
python3 research/playplay/tools/find_copy_anchor.py
```

No sobrescribir `copy-anchor-148-2026-09-24.json` al redirigir otra corrida.

### probe_cached_audio.py

Windows/Python con `cryptography`. Solo lee prefijos512 de `*/*.file`;
requiere `--cache-dir`, `--keys-json`, `--report`. Fixture
`{"candidates":[{"label":"nombre","key":"32 caracteres hex"}]}`.
AES-CTR con IV documentado; exige Ogg versión0/BOS en offset167 y paquete
`01vorbis` después de tabla de segmentos, no solo el texto OggS.

```powershell
& $ppPython .\probe_cached_audio.py --cache-dir "$env:LOCALAPPDATA\Spotify\Data" --keys-json cache-probe-captured-context-148-2026-09-24.json --report cache-nueva.json
```

Salida 0 si algún match, 1 si ninguno; configuración inválida/excepciones son
errores, no negativos criptográficos. Archivos bloqueados se registran en
`errors` y omiten. La primera corrida usó
`cache-probe-candidates-148-2026-09-24.json` (dos candidatos) y la segunda el
fixture del comando (uno nuevo). 102/116 prefijos, cero matches, un bloqueo por
corrida. Hubo comprobación sintética inline (clave correcta, incorrecta, prefijo
corto); no hay un archivo de tests persistente para esas tres comprobaciones.

## Análisis estático y procedencia

`find_copy_anchor.py` busca la firma de copia con wildcards en el dump148.
`analyze_calls_148.py` y `analyze_calls_148_full.py` buscan calls relativos;
`disasm.py` delimita ventanas de desensamblado. Son comprobaciones offline del
binario exacto; no prueban por sí solas que un candidato sea una clave AES.
Las recetas, resultados y pasos `.pdata`/xref están en el
[playbook](../docs/RVA_DISCOVERY_PLAYBOOK.md).

Los scripts de captura externa token148 se conservan en
[token_capture_148](token_capture_148/README.md). Requieren revisión del PID y
los permisos antes de usar: sus notas registran procedencia, no validación de
un despliegue automático.

El prototipo HTTP148 que falló se conserva como
[fuente de ensayo](../runs/20260924-discovery-148/source/flask_frida_server_final.py).
Su contrato parcial, excepciones y selección de los primeros16 bytes se
explican en [VALIDATION_148](../docs/VALIDATION_148_2026-09-24.md).
Para servicio actual usar exclusivamente [HTTP_DFA_SERVICE](../docs/HTTP_DFA_SERVICE.md).

`extract_dll.py` conserva la utilidad genérica de extracción desde instalador.
No contiene RVAs ni valida automáticamente que el PE obtenido sea el148 esperado;
no se ha repetido esa adquisición en esta revisión. `trace_dump.py` se descarta
como herramienta activa: importaba tamaños/runtime de un paquete externo sin
validación para148. La emulación con evidencia reproducible está en
[la corrida Unicorn](../runs/20260924-unicorn-feasibility/manifest.json).
