# Viabilidad de Unicorn sin Spotify vivo — 2026-09-24

## Dictamen y alcance

La emulación local es una vía viable para continuar: **generador y DFA funcionan
offline con los contextos guardados de dos recursos**. Todavía no está demostrado
el recorrido desde una nueva clave ofuscada hasta AES sin contexto capturado.
El `unicorn_harness.py` externo es un prototipo incompleto, no un extractor listo.
Se conserva [su fuente original](../runs/20260924-unicorn-feasibility/source/original_unicorn_harness.py)
como evidencia de los defectos descritos abajo, no como herramienta validada.
Los ensayos de evaluación están separados en
[runs/20260924-unicorn-feasibility](../runs/20260924-unicorn-feasibility/manifest.json).

Todas las pruebas fueron locales. No se accedió a Windows ni a Spotify, Frida,
credenciales o red de licencias. Se utilizó Unicorn **2.1.4** del venv existente,
el dump 148 y capturas históricas. El dump es una imagen obtenida previamente de
un proceso inicializado; no es el DLL original de instalación. El éxito con ese
dump y snapshots no demuestra arranque frío desde un PE original.

## Resultado experimental

| Prueba | Resultado observado |
|---|---|
| Stream desde snapshot de 740 B | 2 recursos × 2 bases: 4/4 bloques idénticos a captura; 993 instrucciones por llamada |
| DFA sobre esos contextos en Unicorn | 16 alteraciones por recurso, en offsets `0xa0..0xaf`; ambas AES recuperadas por solver offline |
| Verificación independiente | Ambas AES coinciden con referencias y descifran sus prefijos de 4096 B; Ogg/Vorbis con CRC válido |
| Init desde descriptor28, sin TEB | 4/4 intentos fallan en lectura `gs:[0x30]`, RVA `0x20f261c` |
| Init con TEB/PEB mínimos | Ambos casos avanzan hasta instrucción rechazada `vmovdqu ymm0, [rdx]`, RVA `0x177825d` |
| Init/stream con TEB y stub de copia identificado | Avanzan, pero ambos alcanzan el límite de ejecución en RVA `0x1f6a51e`; no se obtiene bloque validado |
| Constructor `0x4a0268`, ABI corregida, base artificial | Falla tras 47 instrucciones al leer un puntero absoluto del dump |
| Mismo constructor, base indicada por el dump | Avanza 120 instrucciones y se detiene al intentar ejecutar `0x7ffa4974cb40`, dirección externa que el harness etiqueta HeapAlloc |

Las pruebas estrictas paran en el primer acceso no mapeado; no crean páginas
vacías ni simulan éxito de funciones desconocidas. Cada llamada tiene límite de
2 segundos y 2 millones de instrucciones. La variante de copia sustituye solo
la función identificada `0x17780e0`, leyendo/escribiendo sus buffers y simulando
su retorno Win64; los demás accesos siguen fallando. El límite alcanzado no
demuestra un bucle infinito ni determina por sí solo la dependencia restante.

Los primeros bloques y las trazas de fallos se producen sin suministrar AES al
emulador. Un evaluador separado resuelve las trazas y después compara claves y
contenido. La salida positiva **depende del snapshot inicial por recurso**: para
una licencia nueva sigue faltando construir ese estado sin Spotify vivo.

Evidencia primaria:

- [Sonda estricta: 10 casos](../runs/20260924-unicorn-feasibility/raw/strict-probe.json).
- [Fallos generados por Unicorn y prueba TEB](../runs/20260924-unicorn-feasibility/raw/context-faults.json).
- [Recuperación de claves y control de contenido](../runs/20260924-unicorn-feasibility/raw/dfa-verification.json).
- [Prueba con copia identificada](../runs/20260924-unicorn-feasibility/raw/init-copy-probe.json).

## Defectos concretos del harness externo

1. **Contrato equivocado.** Invoca `0x4a0268` como si fuera la transformada
   `vm_obj, obfuscated, derived, init`. El constructor ensayado recibe
   `request256, obfuscated16, auxiliary4, null`; requiere `request+0x70=1` para
   omitir callback. R8 no es el buffer de salida28. Esa salida está en el stack
   del caller y debe copiarse al retorno de `0x49eaa4` (callsite `0x4a041d`,
   retorno `0x4a0422`) antes de perder su vida útil. Ver
   [contratos](PLAYPLAY_ARCHITECTURE_NOTES.md).
2. **Base incorrecta para un dump ya relocalizado.** El PE del archivo cuyo hash
   termina en `d89f2cc` contiene ImageBase `0x7ff9b7da0000`, no `0x180000000`.
   Mapearlo plano es apropiado para su layout RVA=offset, pero moverlo de base
   sin reparar referencias absolutas no lo es. La sonda verifica esta diferencia.
   No aplicar `get_memory_mapped_image()` como si fuera un PE original: sus
   offsets de sección en disco no describen el layout plano conservado.
3. **Imports dependientes de una sesión histórica.** El dump contiene direcciones
   externas absolutas. No son RVA de Spotify ni una tabla estable entre máquinas.
   La dirección que el prototipo etiqueta HeapAlloc debe identificarse mediante
   procedencia del import antes de registrarla como API validada.
4. **Stubs sin contrato.** Ante fetch desconocido rellena una página con `RET`;
   ante otras APIs intenta `memcpy` y devuelve RCX; ante lecturas inválidas crea
   memoria a cero. Esto puede esconder dependencias y producir datos inventados.
   «Terminó sin crash» deja de ser criterio útil con esas sustituciones.
5. **Estado de Windows incompleto.** Tener TEB/PEB mínimos no implementa TLS,
   globals, callbacks ni excepciones del runtime. La ruta AVX observada requiere
   configuración compatible o una sustitución precisa de copia. Las excepciones
   SEH/C++ siguen siendo una dependencia a investigar, no la causa demostrada de
   todos los fallos encontrados aquí.
6. **Control de ejecución insuficiente.** El prototipo no pone límite a
   `emu_start`, no verifica retorno real ni compara la salida con un control
   positivo. Su ruta de archivo depende del cwd. Además usa una entrada histórica
   E en vez de uno de los controles positivos token148.

Unicorn emula CPU; no suministra automáticamente el runtime de Windows. La
[FAQ oficial de Unicorn](https://github.com/unicorn-engine/unicorn/blob/master/docs/FAQ.md)
explica esa distinción y los límites de ejecución. Es la razón para modelar
dependencias explícitamente, no para simular una respuesta exitosa a cualquier API.

## Procedimiento propuesto

1. Conservar el generador/DFA offline demostrado como control positivo y posible
   etapa final. También permitiría un flujo intermedio donde se captura contexto
   una vez y la recuperación de AES ocurre localmente; ese flujo aún depende de
   una captura por entrada y no cumple independencia completa.
2. Corregir mapeo, ABI, callback y punto de captura del constructor. Usar el hash
   exacto y la base adecuada; detenerse ante cada dependencia nueva con traza.
3. Identificar imports y modelar únicamente sus contratos reales. Priorizar
   asignador, copia y estado requerido por el camino efectivamente ejecutado.
   Para wrappers de descriptor investigar qué datos del entorno consumen; una
   falla con TEB ficticio no prueba que el descriptor sea portable entre hilos.
4. Modelar el runtime y el manejo SEH observados en el build 148, verificando
   cada dirección, tamaño y constante contra su dump y proceso vivo.
5. Cerrar con dos entradas ofuscadas, contextos generados íntegramente por el
   emulador, AES recuperadas por DFA y prefijos con CRC válido. Repetir desde una
   máquina emulada vacía por caso, sin leer snapshots por recurso. Después probar
   licencia nueva y, por separado, el PE original si se requiere arranque frío.

La factibilidad del generador/DFA ya es un resultado experimental. La de la
cadena completa es prometedora pero incierta: no hay base para asignarle un
porcentaje fiable ni describirla como una corrección de dos líneas.

## Reproducción y fuentes

Los scripts del ensayo están en [source](../runs/20260924-unicorn-feasibility/source/).
Para repetir, copiar esa carpeta a una nueva corrida con directorio `raw` vacío;
los escritores son exclusivos y no sobrescriben informes. Desde la raíz del repo,
ejecutar en la nueva ruta: `probe.py`, `context_faults.py` e `init_copy_probe.py`
con `research/playplay/ppvenv/bin/python`; después `evaluate_faults.py` con Python
que tenga cryptography. El evaluador lee las funciones matemáticas de tools y
registra su hash; las fuentes usadas se conservaron en `source/reference`.

La auditoría estática independiente fue realizada por gpt-6-luna/medium; el
coordinador ejecutó y evaluó las pruebas locales. No se cambió el harness ajeno,
el C++ ni el índice Git.
