# Extracción de AES-128 mediante DFA — hito del build 1.2.92.148

El hito principal fue **recuperar K0, la clave AES de contenido de 16 bytes,
sin suministrarla al cálculo de extracción**. La primera recuperación corresponde
al recurso `2f43127d80edc9cd9f12f441e1cb7904b680f9da`; después se confirmó un
segundo recurso token148/v5 y se integró el cálculo en la API Windows de cspot.

Este informe conserva el razonamiento, la secuencia experimental y la evidencia.
El resultado no demuestra que K0 nunca aparezca en memoria: las búsquedas
lineales fueron negativas solo en las regiones e instantes examinados.

## Qué aporta recuperar la clave

La licencia PlayPlay contiene `obfuscated_key` de 16 B y `b4_seq` de 4 B. El
VM transforma esa entrada en un descriptor opaco de 28 B; un inicializador crea
el contexto del generador de bloques de audio. Se comprobó que, para las dos
licencias de control, sus salidas producen el stream de AES-128-CTR esperado.
El token PlayPlay del request, la entrada ofuscada, el descriptor y K0 cumplen
funciones diferentes; un candidato interno de 16 B no es automáticamente K0.

Obtener solamente el stream permitiría descifrar lo que el generador produzca,
pero obligaría a conservar acceso a él para más bloques. Con K0 se puede usar
AES-128-CTR localmente en cspot y aprovechar las implementaciones de Linux o
ESP32. La extracción actual necesita Spotify Windows; el descifrado posterior
de los bytes de audio no necesita enviar cada bloque al servicio. Reproducción
completa y ESP32 siguen pendientes de validación.

## Cómo se llegó al punto de inyección

La [procedencia de RVAs](docs/RVA_DISCOVERY_PLAYBOOK.md) conserva el detalle:

1. La firma de copia localizó `0x17780e0`. Los trazadores relacionaron la copia
   candidata `0x49f904`, su caller `0x49f854` y la salida final de **28 B** en
   `0x49f961`. Los primeros 16 B del descriptor no superaban los controles AES.
2. Se identificó el constructor `0x4a0268`, que permite ejecutar la transformada
   con una solicitud privada de 256 B y callback deshabilitado (`+0x70=1`). La
   salida se captura al retorno de `0x49eaa4`, antes de perder el stack del caller.
3. Seguir la ruta de reproducción llevó a `0xd9e2e4` (inicialización desde
   descriptor28) y `0xd9d0f0` (generador). El snapshot previo a un bloque mide
   **740 B**. Reservar 4096 B en el harness es una elección de capacidad, no el
   tamaño demostrado de la estructura.
4. El replay de un contexto natural reprodujo su bloque real. Las dos licencias
   token148/v5 permitieron después comparar 4096 B por recurso con AES-128-CTR
   y verificar Ogg/Vorbis y CRC. Con ese control positivo se investigaron fallos.

[Ensayos de ABI y descubrimiento](docs/VALIDATION_148_2026-09-24.md),
[control de contenido](docs/TOKEN148_ONESHOT_2026-09-24.md) y
[capturas limpias](docs/CLEAN_CAPTURE_148.md) delimitan esas etapas. El origen
inicial de los RVAs VM/init no quedó archivado como buscador; los callers y los
controles posteriores confirman su uso, sin reconstruir ese origen ausente.

## Metodología del ataque

Se guarda el contexto antes del primer bloque. Una ejecución sin alterar produce
el bloque correcto C; antes de cada prueba se restaura el snapshot, se cambia un
bit y se vuelve a ejecutar el generador para obtener C'. Restaurar el contexto
es necesario porque cada llamada avanza el contador/estado de audio.

El primer barrido está conservado en [dfa_data.json](docs/dfa_data.json), seguido
por [dfa_sweep.json](docs/dfa_sweep.json). El ensayo reproducido en Unicorn utiliza
16 perturbaciones por recurso en los offsets `0xa0..0xaf`. Los patrones de cuatro
bytes alterados permiten aplicar el modelo DFA AES anterior al último MixColumns.
Eso describe el efecto observado; no identifica inequívocamente esos bytes como
un key schedule estándar ni revierte toda la codificación interna del contexto.

El [solver inicial](tools/dfa_solver.py) y la función `solve_dfa` de
[extractor.py](tools/extractor.py) agrupan los fallos por estas posiciones de
salida, teniendo en cuenta ShiftRows:

| Grupo | Índices de bytes |
|---|---|
| 0 | 0, 13, 10, 7 |
| 1 | 4, 1, 14, 11 |
| 2 | 8, 5, 2, 15 |
| 3 | 12, 9, 6, 3 |

Para cada candidato de byte k de K10 se calcula
`InvSBox(C[i] xor k) xor InvSBox(C'[i] xor k)`. Las cuatro diferencias deben
ajustarse al mismo error no nulo y a los multiplicadores de MixColumns. Se
intersectan los candidatos compatibles con varios fallos hasta obtener una
solución por grupo. Cuatro grupos determinados proporcionan los 16 B de K10.
[reverse_key_schedule.py](tools/reverse_key_schedule.py) invierte la expansión
AES-128 para recuperar K0. La AES de referencia solo interviene después, en la
verificación independiente, no en estas ecuaciones.

## Resultados y comprobación independiente

| Recurso | K10 recuperada | K0 recuperada |
|---|---|---|
| `2f43127d80edc9cd9f12f441e1cb7904b680f9da` | `398cc5af7975a2ed0c547d6a005902e1` | `a503a84c1dc9271460cc13f142e0bae2` |
| `1a8e5b04837957617162724232b0c96922222447` | `0966860b7226db38272cade4cfeb5b0d` | `c3206271b4c70fff8e4ac3993c4dae8a` |

El [ground truth actual](data/ground-truth-vectors.json) conserva file_id,
obfuscated_key, b4_seq, AES, K10, IV, primer bloque nativo y fuentes con hashes.
La [evaluación DFA](runs/20260924-unicorn-feasibility/raw/dfa-verification.json)
compara las claves recuperadas con referencias independientes. Ambas reproducen
el bloque nativo `AES-ECB(K0, IV)` y descifran los prefijos de 4096 B con una
primera página Ogg/Vorbis y CRC válidos.

Las [trazas de fallos](runs/20260924-unicorn-feasibility/raw/context-faults.json)
y el [manifiesto](runs/20260924-unicorn-feasibility/manifest.json) permiten repetir
la evaluación offline. Unicorn reproduce generador y DFA **desde snapshots por
recurso**; no se ha construido todo el contexto desde una licencia nueva sin
Spotify vivo. Conservar esta distinción evita atribuir al hito un arranque
standalone que todavía no se demostró.

## Automatización y uso actual

La implementación matemática de `extractor.py` alimenta el servicio actual.
La [API HTTP](docs/HTTP_DFA_SERVICE.md) añade preflight de versión/hash/firmas,
`b4_seq` obligatorio, aislamiento del worker, timeout y verificación del bloque
nativo antes de devolver AES16. La [corrida HTTP](runs/20260925-http-dfa-service/EXPERIMENT.md)
validó las dos licencias repetidamente, con Spotify recién abierto y después de
parar/arrancar ordenadamente el servicio. cspot conserva ambos campos de licencia
y su CLI compiló.

El CLI experimental de `extractor.py` sigue disponible, pero tiene controles de
operación menos completos que el servicio; no se debe interpretar su rótulo
«Producción» como validación adicional. `extract_aes_dfa.py` es un placeholder
que imprime valores fijos y **no ejecuta un ataque**; no es evidencia del hito.
Los capturadores experimentales se inspeccionan antes de usar y sus RVAs solo
corresponden al binario148 cuyo hash está en [FACTS](docs/FACTS.md).

Para comprobar el ground truth sin red, Windows ni nuevas capturas:

```sh
python3 research/playplay/tools/check_ground_truth_148.py
python3 research/playplay/tools/check_direct_aes_148.py
```

## Alcance y siguiente trabajo

DFA es una vía demostrada para las dos licencias de control. No acredita todavía
cualquier pista, cuenta o build. Falta probar una licencia nueva y reproducción
completa de cspot/ESP32. La [búsqueda directa](docs/DIRECT_AES_SEARCH_148.md)
conserva alternativas para localizar un punto de K0; hallar esa instrucción y
su puntero podría permitir una captura directa siguiendo el modelo externo de
`another-unplayplay`, una vez resuelto también el entorno de emulación148.
