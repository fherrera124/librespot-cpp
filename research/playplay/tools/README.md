# Referencia detallada de herramientas PlayPlay

Para elegir herramienta rápidamente: [CATALOG](../CATALOG.md) y
[STATUS](../STATUS.md). Este documento conserva comandos y anchors históricos.
La prueba token148 es el control positivo; los ensayos E siguientes son antecedentes.

**Servicio actual (2026-09-25):** [API HTTP de AES16](../docs/HTTP_DFA_SERVICE.md).
`playplay_dfa_service.py` + `playplay_dfa_rpc.js` usan Frida/DFA y exigen
`obfuscated_key`/`b4_seq`. `verify_playplay_service.py` verifica las respuestas
contra licencias y contenido desde Linux. El servidor antiguo en resources
y `validate_lan_service.py` (vectores E sin b4_seq) no implementan este contrato.

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

**Actualización posterior:** tres herramientas nuevas validan el stream nativo
token148/v5 y contenido, **sin servicio LAN**. No extraen AES16 todavía.
Comandos, fixtures e informes en [TOKEN148_ONESHOT](../docs/TOKEN148_ONESHOT_2026-09-24.md).

- `probe_token148_once.py`: Linux, requests/cryptography, login desde session.json,
  una licencia nueva y opcionalmente prefijo CDN; no persiste tokens de cuenta.
- `check_fresh_license_148.js`: Frida mediante el runner existente, b4_seq explícito,
  de 1 a 256 bloques por ejecución y comparación de dos ejecuciones.
- `verify_token148_content.py`: offline, cryptography; compara todos los bytes
  contra AES-128-CTR, descifra el prefijo y comprueba Vorbis/CRC Ogg. Exit0=éxito.

El runner histórico sigue devolviendo 2 para estos eventos alternativos aunque
terminen correctamente. Usar el verificador offline como resultado del ensayo.

| Entorno | Dependencias / uso |
|---|---|
| Linux, Python 3 | Biblioteca estándar para HTTP, sus tests y buscador de firma |
| Linux, `research/playplay/ppvenv/bin/python` | `pefile`, `capstone` para análisis estático; receta en el playbook |
| Windows, Python 3.12 | `frida`, `psutil` para runner; `cryptography` para caché; `flask` solo servidor histórico |
| Scripts JS | Se cargan **con Frida**, no con Node ni directamente con Python |
| Paquete `unplayplay` local | Hash gate modificado; no usar como referencia canónica sin contrastar su código/config |

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

Copiar el runner, JS elegido y fixtures a una misma carpeta Windows. Ejemplo
desde la raíz del repositorio (la clave SSH es obligatoria):

```sh
scp -i ~/.ssh/win_claude research/playplay/tools/validate_windows_vm.py \
  research/playplay/tools/check_key_pipeline_148.js \
  research/playplay/data/ground-truth-vectors.json \
  research/playplay/data/key-pipeline-controls-148-2026-09-24.json \
  francisco.herrera@10.16.150.154:
ssh -i ~/.ssh/win_claude francisco.herrera@10.16.150.154
```

En **PowerShell**, situado en `C:\Users\francisco.herrera`:

```powershell
$ppPython = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
$ppPid = 64216 # Sustituir por el PID principal recién verificado.
$ppRun = @("--pid", "$ppPid", "--vectors", "ground-truth-vectors.json",
           "--timeout", "180", "--exceptions", "propagate")
```

Mantener SSH vivo durante la ejecución en primer plano. Recuperar el informe
con `scp` al terminar y usar siempre un nombre nuevo; los escritores abren en
modo exclusivo. Al cerrar, verificar que el ensayo se desadjuntó. Al corte de
esta sesión **no quedó un ensayo esperando ni servidor validado activo**.

## Prueba preferida sin cambiar de cancion

**Control histórico E.** Para token148 usar la entrada token148-control del
[catálogo](../CATALOG.md). Para extracción preparar el capturador limpio del PLAN.
La expresión «preferida» de este anchor conserva compatibilidad con enlaces viejos.

```powershell
& $ppPython .\validate_windows_vm.py @ppRun --script check_key_pipeline_148.js --script-data key-pipeline-controls-148-2026-09-24.json --report pipeline-nuevo.json
```

En el proceso 148 ya inicializado no necesita otra reproducción: crea el runtime
mediante `0x4a0268`, deshabilita callback de un objeto local y ejecuta dos veces
13 entradas. Debe haber 13 eventos `pipeline_control`, cuatro bloques iguales
por par, `done` y ningún error. El informe histórico cumple eso; sus comparaciones
con AES/IV estándar son negativas. No se probó arranque frío.

**Limitación del runner:** para este diagnóstico puede devolver **2** y
`inconclusive_execution_error` aunque se hayan completado los controles. Ver
la siguiente sección antes de interpretar el resumen.

## Runner: validate_windows_vm.py

Requiere `--pid`, `--vectors`, `--report`; timeout por defecto 120 s.
Verifica que PID sea Spotify principal, inyecta `vectors`, adjunta/carga JS,
recoge eventos y finalmente descarga y desadjunta. Opciones:

| Opción | Uso y límite |
|---|---|
| `--script archivo.js` | Diagnóstico alternativo. Sigue necesitando `--vectors`, aunque ese JS no los use |
| `--script-data archivo.json` | Inyecta el JSON como `diagnosticData`; requiere `--script` |
| `--exceptions propagate` | Cambia **solo el JS integrado**; scripts alternativos declaran sus propias opciones NativeFunction |
| `--advance-track` | Añade `advance_spotify_track.js` desde la carpeta del runner; copiar también ese archivo |
| `--timeout segundos` | Tiempo máximo de espera del runner; no prueba que el hook haya ocurrido |
| `--report nuevo.json` | Registra fuente compuesta SHA256, hash de vectores, PID, eventos y evaluación |

Sin `--script` usa el **ensayo inicial superado**: reentrada en `onEnter`,
lectura de 24 bytes finales y selección de 16 candidatos. Su default
`exceptions=steal` falló con system error; con propagate falló el control del
buffer final. Conservar como diagnóstico histórico, no como extractor preferido.
El texto de consola «Play a different song...» se imprime incluso para scripts
automáticos; no implica que el pipeline necesite una canción.

La evaluación genérica solo entiende 11 eventos `result` y
`natural_output`/`control_replay`:

- 0: 11 candidatos iguales a AES esperadas y control natural válido.
- 1: lote completo, control válido y diferencias candidatas.
- 2: error, lote incompleto o control fallido.

`extract_vm_key_148.js` usa ese contrato. Los **otros JS diagnósticos** escriben
en `events` sin poblar `results`; su resumen da completed=0/control_pass=false
y código 2 incluso si tuvieron éxito. Para ellos comprobar ausencia de `error`
y `fatal`, evento `done`, cantidad esperada y campos propios de cada ensayo.
El código 2 también puede ser un error real: no ignorarlo indiscriminadamente.
Receta de evaluación offline en [el informe](../docs/VALIDATION_148_2026-09-24.md#comprobar-los-resultados-sin-windows).

Los informes iniciales conservan `mode: onEnter` incluso para JS alternativos;
`mode` se corrigió después. `script_sha256` corresponde al código **compuesto**,
incluidos fixtures/helper, no al archivo JS aislado. Scripts que evolucionaron
de 24 a 28 bytes o de uno a cuatro bloques no reproducen byte por byte el
formato de informes anteriores.

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

Es la prueba preferida descrita arriba. El fixture es un array de 13 objetos:
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

### validate_lan_service.py

Cliente HTTP stdlib Linux. `--url` obligatorio, `--vectors` por defecto
fixture del repo, `--timeout` 10 s, `--repeat` 1; `--report` guarda JSON.
No inicia servidor ni adjunta Frida, no necesita credenciales. Ignora proxies
del entorno, rechaza redirecciones y limita respuesta a 64 KiB. Se detiene al
primer error de transporte/protocolo, no ante un simple mismatch.

```sh
python3 research/playplay/tools/validate_lan_service.py \
  --url http://10.16.150.154:8080/deob --repeat 2 \
  --report research/playplay/docs/lan-validacion-nueva.json
```

Salida 0: todos coinciden; 1: diferencias; 2: error. Conserva fecha, hash de
vectores y resultados por recurso/version. Dos pasadas completas son 22; el
ensayo real terminó en la primera petición HTTP500. **El endpoint está detenido
y sin validar**; este comando es para cuando se repare el servidor.

### test_validate_lan_service.py

Cuatro tests unittest con servidor HTTP simulado: caso correcto repetido,
diferencias y errores de servicio/formato. No prueban Spotify ni el VM.

```sh
python3 -m unittest discover -s research/playplay/tools -p test_validate_lan_service.py -v
```

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

## Servidor ensayado, conservado como prototipo

[flask_frida_server_final.py](../resources/flask_frida_server_final.py) requiere
Python Windows con Flask/Frida/psutil. Se ejecutó **sin argumentos**, con Python
en primer plano por SSH; copia remota `playplay_validation_148_20260924.py`.
Busca proceso principal, espera una transformada natural y publica
`POST /deob` en `0.0.0.0:8080`: JSON `obfuscated_key`, respuesta `aes_key`.
No tiene `file_id`, caché ni contrato de producción validado.

Conserva args[3] como puntero, usa excepciones por defecto y devuelve primeros
16 del buffer final. Dio HTTP500/system error. Para reproducir exclusivamente
ese fallo histórico se invocaba:

```powershell
& $ppPython .\playplay_validation_148_20260924.py
```

No es el procedimiento recomendado para retomar. La prioridad es AES y contrato
nativo; luego reparar servidor. No relanzar tampoco `flask_frida_667.py`, servicio
previo remoto con offsets diferentes. [Informe](../docs/VALIDATION_148_2026-09-24.md)
conserva hashes/logs y limpieza.

## Herramientas anteriores: inspeccionar antes de reutilizar

Estas no son nuevos resultados de ejecución de esta sesión. Pueden tener paths
obsoletos, efectos laterales o offsets de otra sub-build. Los DLL actuales están
en **`research/dlls/`**, no en `research/playplay/*.dll`.

| Script(s) | Propósito histórico / cómo considerar su uso |
|---|---|
| `extract_dll.py <instalador.exe> <salida.dll>` | Extrae DLL x64 del overlay LZMA1 de instaladores full compatibles; no garantiza todos los instaladores |
| `vm_check.py <dll>` | Oráculo Unicorn/config del paquete; revisar hash gate local alterado y offsets antes de confiar en resultado |
| `run_vm_485.py [dll]` | Ensayo Unicorn saltando gate; dar ruta explícita `research/dlls/...`; falló con sub-build local |
| `run_vm_483.py [dll]` | Similar; necesita `UPP483_SRC` al código del paquete correspondiente |
| `groundtruth.py` | Recaptura entradas token E; requiere `SP_BEARER`/`SP_CLIENT_TOKEN`, contacta backend y escribe fixture histórico. No ejecutado para este handoff |
| `sweep_versions.py`, `probe_tokenF.py` | Barridos autenticados token/version; revisar endpoints/output/rate limits antes de repetir |
| `find_rip.py`, `find_rip_667.py` | Firmas de puntos de extracción de otras referencias; patrones contrastados contra148 sin coincidencia; no portar offsets a ciegas |
| `find_hooks*.py`, `extract_patterns.py`, `find_refs.py`, `find_667.py`, `find_cxx.py`, `find_context.py` | Buscadores de firmas/xrefs; revisar rutas/layout/hashes, no prueban AES |
| `analyze_calls_148*.py`, `disasm*.py`, `exports.py` | Análisis estático; el desensamblado lineal puede perder ramas por ofuscación |
| `pad_dll.py`, `patch_sec.py`, `patch_485_test.py` | Preparan/modifican binarios de prueba; no aplicarlos al original sin leer destinos |
| `dump_ctx.py`, `dump_trigger.py`, `trace_ctx.py`, `trace_dump.py`, `trace_485.py`, `trace_667*.py` | Captura/trazas Frida históricas; PID/ABI/RVAs no se presumen vigentes |
| `test_derived.py`, `test_dummy_ctx.py`, `test_real_ctx.py` | Experimentos de contexto/resultado, no suite de aceptación |
| `test_483.ps1`, `test_apiset.ps1`, `test_arch.ps1`, `test_dll.cpp` | Harness de carga nativa histórica, no solución standalone demostrada |
| `resources/linux_local_scripts/validate_all_in_js.py` | Inspeccionado: adjunta a varios Spotify y no agrega evaluación fiable; no usar como validador actual |
| `resources/linux_local_scripts/validate_sync_fixed.py`, `validate_vectors.py` | Validadores históricos inspeccionados; preceden controles/límites actuales; usar runner explícito |
| `resources/linux_local_scripts/send_*.ps1` | Helpers históricos de interfaz; inspeccionar selección de ventana/proceso |
| `resources/linux_local_scripts/update_summary.py`, `fix_loop.patch`, `patch.cpp` | Ediciones históricas; no ejecutar/aplicar para reconstruir estado actual |
| `archive/*` | Ensayos descartados de carga/mapeo y servidor; conservar como historia, no despliegue |
| `resources/dump_playplay.py` | Addon MitM que sobrescribe body de request en archivo fijo; no usado en controles actuales. [Notas y límites](../resources/mitm_setup_notes.md) |

Los comandos inline de análisis estático y comparación AES usados en la
investigación quedan reproducibles en el [playbook](../docs/RVA_DISCOVERY_PLAYBOOK.md)
y [validación](../docs/VALIDATION_148_2026-09-24.md). No consultar credenciales ni
contactar el backend para repetir esas comprobaciones offline.
