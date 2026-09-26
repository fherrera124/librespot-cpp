"""Valida un Spotify.dll contra los vectores publicados y despues prueba
nuestras keys ofuscadas de token E.  No usa red ni credenciales.
uso: ppvenv/bin/python vm_check.py /ruta/a/Spotify.dll"""
import hashlib
import sys
from pathlib import Path

from unplayplay import PLAYPLAY_TOKEN, SP_CLT_VERSION, KeyEmu
from unplayplay.consts import SP_CLT_SHA2

# (obfuscated, aes esperada) publicados por el paquete - token C
VECTORS = [
    ("eff09d518b4cad9a8feaf11b1a245870", "a503a84c1dc9271460cc13f142e0bae2"),
    ("e0934811ccbe830d3ea3330bc6a4bd8d", "c3206271b4c70fff8e4ac3993c4dae8a"),
    ("c62a616307aa090a8b718cd9d6a7cd33", "8d86fb522c00729f35b34d60b165b922"),
    ("558bda51efa8c558ad54e87dcea4234a", "0fd3998b706247b3474b2d3cf6d8e31f"),
    ("81f6c97072e83efe106d8f372216ed1b", "4d442c155f9a95258f613a89be957be9"),
]

# nuestras, token E (01f62e56...), file f5eb2b3e7a3798b4a55369bd3cc840fceddebb94
OURS = [
    ("version=2", "52c714004e70b11f8ee9336c7d9c373c"),
    ("version=5", "6465252c78c42764fac7b705ac93b7c7"),
]

path = Path(sys.argv[1])
data = path.read_bytes()
digest = hashlib.sha256(data).digest()
print(f"archivo  : {path}  ({len(data):,} bytes)")
print(f"sha256   : {digest.hex()}")
print(f"esperado : {SP_CLT_SHA2.hex()}  (v{SP_CLT_VERSION})")
if digest != SP_CLT_SHA2:
    print("\nNO COINCIDE - el paquete lo va a rechazar. Hace falta exactamente ese build.")
    sys.exit(1)
print("coincide.\n")

emu = KeyEmu(path)
print(f"token del VM: {PLAYPLAY_TOKEN.hex()}\n")

ok = 0
for obf, want in VECTORS:
    got = emu.get_aes_key(bytes.fromhex(obf)).hex()
    good = got == want
    ok += good
    print(f"  {'OK  ' if good else 'FALLA'} {obf} -> {got}" + ("" if good else f"  (esperada {want})"))
print(f"\nvectores publicados: {ok}/{len(VECTORS)}")
if ok != len(VECTORS):
    print("el emulador no reproduce sus propios vectores: parar aca.")
    sys.exit(1)

print("\n--- nuestras keys ofuscadas (token E) ---")
for label, obf in OURS:
    print(f"  {label:10} {obf} -> {emu.get_aes_key(bytes.fromhex(obf)).hex()}")
print("\nPara saber si alguna sirve hay que probarla contra un chunk real del CDN\n"
      "(descifrar AES-128-CTR y buscar 'OggS' en el offset 167).")
