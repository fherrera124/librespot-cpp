# Localizar y contrastar RVAs: evidencia 148 y portabilidad

El build148 tiene rutas ejecutadas y controles de repetibilidad; **no un
extractor AES en producción**. Los nombres VM/init/stream son descriptivos.
La procedencia original de49cb88/49eaa4 no quedó guardada como buscador; esta
sesión confirmó ejecución y relaciones, sin inventar una derivación por firma.

[FACTS](FACTS.md) fija hashes y tabla de RVAs. Este playbook reproduce búsquedas
estáticas utilizadas y explica qué faltaría para otro binario.

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

La referencia del buscador es la rutina de copia en sub-build485 RVA16aa770.
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

La siguiente búsqueda enumera bytes E8 cuyo destino rel32 es la transformada.
**Son candidatos**: datos u operandos también pueden contener E8; hay que
desensamblar el caller y verificar frontera de instrucción.

```sh
python3 - <<'PY'
from pathlib import Path
import struct
raw = Path("research/dlls/Spotify_1.2.92.148_dump.dll").read_bytes()
target = 0x49eaa4
offset = 0
while True:
    offset = raw.find(b"\xe8", offset)
    if offset < 0 or offset + 5 > len(raw):
        break
    dest = offset + 5 + struct.unpack_from("<i", raw, offset + 1)[0]
    if dest == target:
        print("candidate call RVA", hex(offset), "return", hex(offset + 5))
    offset += 1
PY
```

Se confirmó call4a041d/return4a0422 dentro de4a0268 y call4a0407 al init49cb88.
El backtrace natural corresponde a esa ruta. Los controles runtime fresco
ejecutaron ese caller, no una inicialización inventada desde cero.

Para seguir un callback: localizarlo en la captura de4a0494, buscar su entrada
.pdata, desensamblar branches/calls y comprobar sus argumentos. La sesión siguió
precarga63fda4→640844 y reproducciónd8dfb8→d90808→d9b55c→d9ce94→d9d088.
Las relaciones estáticas no sustituyen observar el receptor virtual efectivo:
en esta sesión ese receptor de precarga no fue capturado.

## Portar a otro build: procedimiento condicionado

1. Registrar SHA256 **del binario exacto** y layout. Los 483/485 locales son
   sub-builds diferentes de los hashes canónicos de unplayplay. Desactivar el
   hash gate no vuelve válidas sus VAs.
2. Usar una referencia cuyo código en las VAs publicadas esté confirmado.
   Las VAs canónicas485 0x1803e42ac/0x1803e6398 no se presumen correctas para
   nuestro `Spotify_1.2.88.485_g1012a6e0.dll`.
3. Derivar firmas a partir de instrucciones completas; wildcard solo offsets
   que se puedan justificar. Un prólogo corto no identifica una función por sí
   solo. Los patrones `find_rip.py`/`find_rip_667.py` no coincidieron en148.
4. Comprobar xrefs, tamaños/ABI, SEH e inicialización. Strings, diff binario y
   tablas .pdata ayudan cuando no hay firma única; no hay receta automática
   comprobada para667 en esta sesión.
5. Instrumentar exclusivamente el proceso principal, con verificaciones de
   firma, hooks acotados y sin bloquear `onEnter`. Restaurar estado/copies de
   inicializador o demostrar un constructor apropiado para ese build.
6. Exigir control natural positivo de ejecución, luego control independiente de
   AES/contenido. Fallar vectores antes de demostrar extracción no determina
   incompatibilidad de token ni error de RVA.
7. Registrar código, patrones, hash, logs, versiones y evaluación. Actualizar
   FACTS/arquitectura/plan; modificar el servidor solo después de validar AES.

## Casos resueltos y límites al cierre

| Caso | Evidencia alcanzada |
|---|---|
| Copia17780e0 | Firma única reproducible y hook dinámico |
| VM49eaa4 + init49cb88 | Callers estáticos y runtime fresco ejecutado; origen del hallazgo inicial no archivado |
| Candidato49f854 | Firma de entrada, trazado de copia, control natural positivo; 0/11 AES |
| Respuesta4a0494 | Dos licencias/callbacks capturados con layout coherente |
| Constructor4a0268 | 13 casos ×2 sobre proceso ya inicializado, callback local deshabilitado |
| Streamd9d0f0 / initd9e2e4 | Firmas, controles con descriptores y bloque natural reproducido |
| AES de contenido | **No resuelto**; no etiquetar ninguno de los anteriores como extractor AES listo |

Los comandos vivos y condiciones para cada script están en
[tools/README.md](../tools/README.md); los resultados completos en
[VALIDATION_148_2026-09-24.md](VALIDATION_148_2026-09-24.md).
