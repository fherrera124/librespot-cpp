"""Extrae el Spotify.dll x64 de un instalador full de Spotify para Windows, en Linux.

El instalador es un PE con un overlay al final:
  [header 0x23 bytes][stream raw LZMA1]
Los ultimos 5 bytes del header (offset 0x1e del overlay) son las propiedades LZMA1:
props(1) + dict_size(4 LE). El stream arranca en overlay+0x23.
Descomprime a ~400 MB: un archivo con entradas <name><size-varint><bytes>.
La entrada real de Spotify.dll es la que tras el nombre trae un varint y luego 'MZ'
(las otras apariciones del nombre son \\0RunWinMain... y el .sig).

Repetible para cualquier instalador full. OJO (delta-build): el DLL que sale de un
instalador full suele ser una SUB-BUILD (sufijo -g<commit>) con VAs distintas a las
que fijan los paquetes/Wavee; sirve para reversear, no necesariamente para el atajo.

uso: python extract_dll.py <instalador.exe> <salida.dll>
requiere: pefile, lzma (stdlib)
"""
import sys, lzma, hashlib, struct, pefile

exe, out = sys.argv[1], sys.argv[2]
data = open(exe, "rb").read()
pe = pefile.PE(exe, fast_load=True)
ovoff = max((s.PointerToRawData + s.SizeOfRawData) for s in pe.sections)
ov = data[ovoff:]

props = ov[0x1e]
dict_size = struct.unpack("<I", ov[0x1f:0x23])[0]
lc = props % 9
lp = (props // 9) % 5
pb = (props // 9) // 5
print(f"overlay@{ovoff:#x}  LZMA1 props={props:#x} lc={lc} lp={lp} pb={pb} dict={dict_size:#x}")

filt = [{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}]
dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt)
blob = bytearray()
stream = ov[0x23:]
try:
    for i in range(0, len(stream), 8 << 20):
        blob += dec.decompress(stream[i:i + (8 << 20)])
except lzma.LZMAError:
    pass  # el raw stream puede no tener end-marker; nos quedamos con lo descomprimido
print(f"descomprimido: {len(blob):,} bytes")


def varint(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1
        r |= (x & 0x7F) << s
        if not (x & 0x80):
            return r, p
        s += 7


found = None
start = 0
name = b"Spotify.dll"
while True:
    j = blob.find(name, start)
    if j < 0:
        break
    p = j + len(name)
    try:
        size, p2 = varint(blob, p)
        if blob[p2:p2 + 2] == b"MZ" and 1_000_000 < size < 100_000_000:
            found = (p2, size)
            break
    except Exception:
        pass
    start = j + 1

if not found:
    sys.exit("no se encontro la entrada Spotify.dll (name+varint+MZ)")

off, size = found
dll = bytes(blob[off:off + size])
machine = pefile.PE(data=dll, fast_load=True).FILE_HEADER.Machine
if machine != 0x8664:
    sys.exit(f"el PE extraido no es x64 (machine={machine:#x})")
open(out, "wb").write(dll)
print(f"escrito : {out}")
print(f"sha256  : {hashlib.sha256(dll).hexdigest()}")
print(f"tamano  : {len(dll):,}  (x64)")
