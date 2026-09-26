# Localizar y contrastar RVAs: evidencia 148 y portabilidad

El build148 tiene rutas ejecutadas, controles de repetibilidad y recuperación
AES16 por DFA validada con dos licencias token148/v5. La
[API Windows](HTTP_DFA_SERVICE.md) la expone a cspot; todavía falta una licencia
nueva y reproducción completa. [Unicorn](UNICORN_FEASIBILITY_148.md) reproduce
contextos guardados, pero no construye uno desde una licencia nueva.
Los nombres VM/init/stream son descriptivos.
La procedencia original de49cb88/49eaa4 no quedó guardada como buscador; esta
sesión confirmó ejecución y relaciones, sin inventar una derivación por firma.

[FACTS](FACTS.md) fija hashes y tabla de RVAs. Este playbook reproduce búsquedas
estáticas utilizadas y explica qué faltaría para otro binario.

## Qué significa reparar o reconstruir el contexto

Hay dos estructuras diferentes. No se encontró un símbolo o RVA dedicado llamado
«reparación de contexto»; usamos esa expresión para describir estos procedimientos:

| Estructura | Problema observado | Procedimiento ensayado en 148 |
|---|---|---|
| Objeto VM, snapshot de 144 B | La transformada muta estado; reutilizar el mismo objeto produjo un crash histórico | Restaurar el snapshot y copiar el inicializador de 16 B para cada replay, o dejar que `0x4a0268` construya un runtime nuevo |
| Contexto del generador, snapshot de 740 B | Cada bloque avanza el estado | Inicializar desde descriptor28 mediante `0xd9e2e4`; para repetir el primer bloque restaurar una copia previa a `0xd9d0f0` |

El snapshot del objeto VM puede contener punteros del proceso. No es una imagen
portable entre reinicios. Los 4096 B reservados por los harness son capacidad
del buffer; el snapshot probado es 740 B. Tampoco es el `CONTEXT` de registros
de Windows que aparece en los experimentos de excepciones.

### Cómo se llegó a las entradas de construcción e inicialización

Esta reconstrucción distingue lo que quedó conservado de lo que falta:

1. **Punto de partida conocido:** `0x49eaa4` (transformada) y `0x49cb88`
   (inicializador VM). Su primer descubrimiento no quedó archivado como buscador;
   no podemos atribuirles retrospectivamente una firma de descubrimiento.
2. **Buscar al constructor real:** las referencias directas a la transformada
   permitieron confirmar `call 0x49eaa4` en `0x4a041d`, dentro de `0x4a0268`.
   El caller llama antes a `0x49cb88`, desde `0x4a0407`. El backtrace natural
   coincide con esa ruta. La búsqueda `E8 + rel32` de abajo devuelve candidatos;
   el desensamblado verifica que son instrucciones y que pertenecen a ese caller.
3. **Reproducir su contrato:** R18/R19 ejecutaron `0x4a0268` sobre una solicitud
   privada de 256 B, a cero, con byte `+0x70 = 1` para omitir callback; input16,
   auxiliar4 y cuarto argumento nulo. El descriptor28 se copió al retorno de
   la transformada antes de que desapareciera el stack del caller. Esto evita
   construir a mano un runtime cuya estructura todavía no está comprendida.
4. **Seguir el consumidor del descriptor:** en `0x4a0494` se capturó el callback
   de reproducción; sus llamadas y saltos llevaron por
   `0xd8dfb8 → 0xd90808 → 0xd9b55c → 0xd9ce94 → 0xd9d088`.
   La relación de esta cadena es estática; no todos sus nodos tienen una traza
   dinámica individual. `.pdata` ayudó a delimitar funciones. `0xd9ce94` es un
   thunk, por lo que no se descartó por carecer de entrada propia en esa tabla.
5. **Distinguir init de stream:** el consumidor `0xd9d088` usa `0xd9e2e4` con
   contexto, descriptor28 y auxiliar4; usa `0xd9d0f0` con contexto y salida.
   Los harness comprobaron la inicialización sobre memoria privada y la
   producción de bloques de 16 B. Las ventanas de desensamblado y los contratos
   enlazados abajo sostienen esta identificación; no hay un buscador automático
   preservado que derive ambos RVA desde cero para cualquier DLL.
6. **Exigir control natural:** R21 copió 740 B antes de un bloque de reproducción.
   El primer replay coincidió con ese bloque; los siguientes cambiaron al avanzar
   el contexto. R20 había rechazado una firma por un byte mal transcrito: se
   corrigió contra el dump, sin desactivar el control de bytes.

Evidencia: [arquitectura y contratos](PLAYPLAY_ARCHITECTURE_NOTES.md),
[R17–R21 y resultados](VALIDATION_148_2026-09-24.md),
[harness del constructor](../tools/check_key_pipeline_148.js),
[harness init/stream](../tools/check_wrapped_keystream_148.js) y
[captura del bloque natural](../tools/capture_native_stream_148.js).
Los informes originales están indexados en el
[manifiesto de descubrimiento](../runs/20260924-discovery-148/manifest.json).
Los ensayos anteriores con entradas E verifican ABI y negativos del 148, pero
sus AES de otra referencia no son ground truth criptográfico token148/v5.

### Qué agregó la captura recuperada y el DFA

Los capturadores recuperados repiten dos entradas ofuscadas sin insertar las AES
de referencia. El informe de contexto guarda descriptor28, estado inicial y estado
después de un bloque para cada entrada. Esto refuerza el contrato de los RVA ya
encontrados; **no es una nueva derivación de sus direcciones**.

La recuperación DFA posterior parte de salidas correctas y alteradas del generador.
El solver obtiene K10 y revierte su expansión a AES128; las dos licencias
token148/v5 comprobaron bloque, prefijo Ogg/Vorbis y CRC. No exige que la
clave aparezca literalmente en el volcado. Tampoco prueba que nunca exista en
claro en otra región o instante del proceso.

Fuentes: [capturas, hashes y uso](CLEAN_CAPTURE_148.md),
[trazas DFA](dfa_sweep.json), [solver](../tools/dfa_solver.py) e
[inversión](../tools/reverse_key_schedule.py). La descripción del ataque está en
[hito DFA](../dfa_attack_results.md). La [búsqueda directa](DIRECT_AES_SEARCH_148.md)
terminó con un negativo limitado a sus ventanas y buffers: no localizó K0..K10
completa ni en mitades, aunque verificó los dos bloques nativos. No implica
ausencia de clave en cualquier otro instante o región.

## Layout antes de interpretar offsets

- Dump local148: captura raw de memoria, **offset=RVA**, hash275a9f…f2cc.
- DLL de instalador: convertir RVA/file offset con secciones PE. Para analizar
  una imagen virtual usar `pe.get_memory_mapped_image()`.
- No decidir el layout únicamente por tamaño parecido a SizeOfImage: es una
  pista, no prueba. Comprobar procedencia, secciones y bytes de código conocidos.
- Dirección en proceso = base viva del módulo + RVA. No usar ImageBase nominal
  0x180000000 ni una base ASLR de la sesión anterior.

## Firma que sí funcionó: rutina de copia

`tools/find_copy_anchor.py` busca sobre el raw148:

```text
48 8b c1 4c 8d 15 ?? ?? ?? ?? 49 83 f8 0f 0f 87 ?? ?? ?? ??
```

La firma se preserva como ancla de la rutina de copia del raw148. Su origen
histórico fue una comparación entre builds; ejecutarla no requiere otro DLL.
Los wildcards cubren desplazamientos relativos, no bytes arbitrarios del prólogo.

```sh
python3 research/playplay/tools/find_copy_anchor.py
```

Resultado único17780e0, hash y patrón en
[copy-anchor-148-2026-09-24.json](copy-anchor-148-2026-09-24.json).
El hook dinámico valida partes fijas antes de adjuntar. La observación encontró
copia candidata call49f904/return49f909 dentro de49f854 y copia final49f961 de
28 bytes. Este hallazgo no identifica automáticamente AES.

## Desensamblado local y límites de funciones

Receta desde raíz del repositorio; usa el venv existente con pefile/capstone.
Lee el dump, verifica hash y consulta la tabla de excepciones directamente por
RVA; evita que un parser mapee dos veces el raw como si fuera DLL en disco.

```sh
research/playplay/ppvenv/bin/python - <<'PY'
from pathlib import Path
import hashlib, struct
import capstone, pefile
path = Path("research/dlls/Spotify_1.2.92.148_dump.dll")
raw = path.read_bytes()
assert hashlib.sha256(raw).hexdigest() == "275a9fd95b629f55bd6a170bbf41deb17f87ca59ff61056116d527611d89f2cc"
pe = pefile.PE(data=raw, fast_load=True)
directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]  # IMAGE_DIRECTORY_ENTRY_EXCEPTION
entries = list(struct.iter_unpack("<III", raw[directory.VirtualAddress:directory.VirtualAddress + directory.Size]))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for rva in (0x49f854, 0x49f8f0, 0x4a0407, 0x4a0494, 0x63fda4,
            0x640844, 0xd8dfb8, 0xd90808, 0xd9b55c, 0xd9ce94,
            0xd9d088, 0xd9d0f0, 0xd9e2e4):
    spans = [(hex(begin), hex(end)) for begin, end, unwind in entries if begin <= rva < end]
    print(hex(rva), "pdata:", spans)
    for ins in md.disasm(raw[rva:rva+0x40], rva):
        print(hex(ins.address), ins.mnemonic, ins.op_str)
PY
```

Cada ventana es una inspección corta, no reconstrucción completa del flujo.
Para empezar una ventana interior confirmar primero frontera de instrucción.
Las funciones ofuscadas pueden tener saltos/datos que rompen desensamblado
lineal; usar branches y entradas .pdata como anclas. D9ce94 es un salto a
d9d088 y no necesita entrada .pdata propia. No exigir prólogo MSVC a todo thunk.

La firma del generador d9d0f0 empieza
`40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 ec`.
El primer ensayo transcribió mal un byte y fue rechazado. Contrastar dump
antes de concluir que cambió el build.

## Encontrar llamadas directas conocidas

`analyze_calls_148.py` enumera bytes E8 cuyo destino rel32 es la transformada
y el inicializador; `analyze_calls_148_full.py` incluye los otros destinos
conocidos. Ambos comprueban el SHA256 del dump 148.
**Son candidatos**: datos u operandos también pueden contener E8; hay que
desensamblar el caller y verificar frontera de instrucción.

```sh
python3 research/playplay/tools/analyze_calls_148.py
python3 research/playplay/tools/analyze_calls_148_full.py
research/playplay/ppvenv/bin/python research/playplay/tools/disasm.py 0x4a0405 --size 0x25
```

Se confirmó call4a041d/return4a0422 dentro de4a0268 y call4a0407 al init49cb88.
El backtrace natural corresponde a esa ruta. Los controles runtime fresco
ejecutaron ese caller, no una inicialización inventada desde cero.
`find_hooks_148.py` no se conserva como diagnóstico: comparaba bytes de otro
build y su salida no validaba hooks 148. El buscador de llamadas y el
desensamblador anteriores operan sobre el hash exacto del raw148.

Para seguir un callback: localizarlo en la captura de4a0494, buscar su entrada
.pdata, desensamblar branches/calls y comprobar sus argumentos. La sesión siguió
precarga63fda4→640844 y reproducciónd8dfb8→d90808→d9b55c→d9ce94→d9d088.
Las relaciones estáticas no sustituyen observar el receptor virtual efectivo:
en esta sesión ese receptor de precarga no fue capturado.

## Portar a otro build: procedimiento condicionado

1. Registrar SHA256 **del binario exacto** y layout. No trasladar RVAs publicados
   para otro hash. Desactivar un hash gate no vuelve válidas sus VAs.
2. Usar una referencia cuyo código en las VAs publicadas esté confirmado.
   [another-unplayplay](https://github.com/cycyrild/another-unplayplay) sirve como
   modelo condicionado de método, no como tabla de RVAs equivalentes al 148.
3. Derivar firmas a partir de instrucciones completas; wildcard solo offsets
   que se puedan justificar. Un prólogo corto no identifica una función por sí
   solo. Los patrones históricos de RIP no coincidieron en148.
4. Comprobar xrefs, tamaños/ABI, SEH e inicialización. Strings, diff binario y
   tablas .pdata ayudan cuando no hay firma única; no hay receta automática
   comprobada para cualquier build nuevo en esta sesión.
5. Instrumentar exclusivamente el proceso principal, con verificaciones de
   firma, hooks acotados y sin bloquear `onEnter`. Restaurar estado/copies de
   inicializador o demostrar un constructor apropiado para ese build.
6. Exigir control natural positivo de ejecución, luego control independiente de
   AES/contenido. Fallar vectores antes de demostrar extracción no determina
   incompatibilidad de token ni error de RVA.
7. Registrar código, patrones, hash, logs, versiones y evaluación. La validación
   AES debe ser independiente de la extracción bajo prueba.

Para cada versión nueva, producir una tabla con **función propuesta, RVA,
bytes de entrada, caller/callsite, argumentos/tamaños, hash del DLL, ensayo y
resultado**. Conservar también los candidatos rechazados. Aprobar cada fila
por sus controles, no por parecido de número, prólogo o nombre del script.
La tabla 148 sirve de referencia funcional; cambiar constantes en el capturador
no sustituye hallar y validar el contrato de la nueva versión.

La cadena de aceptación propuesta es: candidato estático → llamada natural y
argumentos observados → replay sobre memoria privada → bloque reproducido →
control AES/contenido independiente → captura nueva y segundo recurso.
Es un procedimiento para investigar otras versiones; no una portabilidad ya
demostrada. No trasladar direcciones absolutas de imports, snapshots con punteros
ni offsets de prototipos de otros builds a 148.

## Entrega reutilizable entre agentes

La guía oficial de [OpenAI Docs sobre AGENTS.md](https://learn.chatgpt.com/docs/agent-configuration/agents-md)
describe instrucciones por ámbito y precedencia. Aplicamos esa organización con
reglas breves en [AGENTS](../AGENTS.md), detalle técnico en este playbook y recursos
en el [catálogo](../CATALOG.md). OpenAI Docs respalda esa organización de trabajo;
la evidencia de los RVA es exclusivamente la del repositorio citada arriba.

Handoff mínimo para una nueva versión:

```text
Binario exacto: ruta, versión, SHA256, arquitectura, layout raw/PE.
Objetivo: reconstruir VM o inicializar/restaurar generador de bloques.
Conocido: RVA de referencia y evidencias; no asumir equivalencia entre builds.
Candidatos: tabla de RVA/callsite/ABI/firmas y motivos para cada hipótesis.
Validado: captura natural, replay y comparación independiente, con artefactos.
Pendiente: huecos explícitos y siguiente comprobación discriminante.
Recursos: un operador Windows; análisis offline puede continuar en paralelo.
```

Guardar fuentes e informes inmutables conforme a [runs](../runs/README.md).
Un agente nuevo debe poder diferenciar «observado», «inferido» y «propuesto» sin
repetir una sesión completa ni confiar en la frase «funciona» de una bitácora.

## Casos resueltos y límites al cierre

| Caso | Evidencia alcanzada |
|---|---|
| Copia17780e0 | Firma única reproducible y hook dinámico |
| VM49eaa4 + init49cb88 | Callers estáticos y runtime fresco ejecutado; origen del hallazgo inicial no archivado |
| Candidato49f854 | Firma de entrada, trazado de copia, control natural positivo; 0/11 AES históricas E, no ground truth token148 |
| Respuesta4a0494 | Dos licencias/callbacks capturados con layout coherente |
| Constructor4a0268 | 13 casos ×2 sobre proceso ya inicializado, callback local deshabilitado |
| Streamd9d0f0 / initd9e2e4 | Firmas, controles con descriptores y bloque natural reproducido |
| AES de contenido | Dos licencias token148/v5 verificadas por DFA, prefijo Ogg/Vorbis y CRC; API HTTP operativa, licencia nueva pendiente |

Los comandos vivos y condiciones para cada script están en
[tools/README.md](../tools/README.md); los resultados completos en
[VALIDATION_148_2026-09-24.md](VALIDATION_148_2026-09-24.md).
