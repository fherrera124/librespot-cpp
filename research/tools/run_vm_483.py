"""Corre el VM del build 483 sobre el Spotify.dll extraido, salteando el gate de
sha256 (el hash difiere solo por la firma Authenticode; las VAs pueden coincidir).

Paso 1 - VALIDACION: reproduce los 5 vectores publicados del propio build 483.
         Si pasan, las VAs de este DLL coinciden y el emulador es correcto.
Paso 2 - si el paso 1 pasa: prueba los 11 vectores de verdad conocida del token E
         (nuestras keys ofuscadas), en v2..v5, contra su AES key esperada.

No usa red ni credenciales. Solo lee el DLL local.
uso: ppvenv/bin/python run_vm_483.py
"""
import hashlib
import sys
from pathlib import Path

SP = Path("/tmp/claude-1000/-desarrollo-git-librespot-cpp/dcb0d1d2-6a97-44f3-92dc-1da719aa851c/scratchpad")
DLL = SP / "Spotify.dll"           # el x64 extraido del instalador
SRC = SP / "upp483" / "another-unplayplay-b281703e7e" / "src"

# usar el codigo del build 483 (no el 485 instalado en el venv)
sys.path.insert(0, str(SRC))

import unplayplay.consts as consts
# el DLL es otra sub-build de 483: mismo codigo posible, distinta firma -> parchear el gate
real_sha = hashlib.sha256(DLL.read_bytes()).digest()
print(f"DLL sha256   : {real_sha.hex()}")
print(f"paquete 483  : {consts.SP_CLT_SHA2.hex()}")
print(f"gate         : {'coincide' if real_sha == consts.SP_CLT_SHA2 else 'PARCHEADO (difiere; probable firma distinta)'}\n")
consts.SP_CLT_SHA2 = real_sha
# algunos modulos ya importaron la constante por valor
for m in list(sys.modules.values()):
    if getattr(m, "SP_CLT_SHA2", None) is not None:
        try: m.SP_CLT_SHA2 = real_sha
        except Exception: pass

from unplayplay import KeyEmu

# --- Paso 1: vectores publicados del build 483 (token F, verdad del paquete) ---
VEC_483 = [
    ("0694259138997536f3ddcf2be2855c8a", "a503a84c1dc9271460cc13f142e0bae2"),
    ("92f454b10dfeef9789f90932d423c244", "c3206271b4c70fff8e4ac3993c4dae8a"),
    ("85d004ad2d1bc9ed12cf80e5db7cc41a", "8d86fb522c00729f35b34d60b165b922"),
    ("a42fbae85590b9a666e7ea6abfe49e1f", "0fd3998b706247b3474b2d3cf6d8e31f"),
    ("6143db25b8035a2cfe5ece34345176d9", "4d442c155f9a95258f613a89be957be9"),
]

print("cargando emulador (puede tardar unos segundos)...")
emu = KeyEmu(DLL)

print("\n== Paso 1: vectores publicados del build 483 ==")
ok = 0
for obf, want in VEC_483:
    got = emu.get_aes_key(bytes.fromhex(obf)).hex()
    good = got == want
    ok += good
    print(f"  {'OK  ' if good else 'FALLA'} {obf} -> {got}" + ("" if good else f"  (esperada {want})"))
print(f"\n  {ok}/{len(VEC_483)} vectores del 483 reproducidos")
if ok != len(VEC_483):
    print("\n  -> Las VAs de este DLL NO coinciden con las del paquete 483.")
    print("     Es genuinamente otra sub-build (codigo distinto). Hace falta el DLL")
    print("     cuyo sha256 sea 9cafe1ca... , o probar el build 485.")
    sys.exit(1)

print("  -> VAs correctas. Este DLL sirve como emulador del 483.\n")

# --- Paso 2: nuestras keys ofuscadas del token E ---
GT = {
  "a503a84c1dc9271460cc13f142e0bae2": ["628c9a976bedc0e3663bc9c4051f1708",
      "1a50e35c5335f9f0b108e79b5dc98f8a","fa699aa7a870569bfc7bd818ed09bab5","611f9b976f284ecf77c202588b8c1d22"],
  "c3206271b4c70fff8e4ac3993c4dae8a": ["79a7680e258598364d3215881ead3ced",
      "1c94840189ef6e384f5aa185da8a4305","d247cd8e62215a995e5829b9c7e67f7c","9678cc71ae205966f814cb500bb7c7cf"],
  "8d86fb522c00729f35b34d60b165b922": ["f4c60dff695fb265104bc50546d6e48a",
      "4c587f8f3b898b63f60ab0043c14a267","3dfba174e5779b38c5e97d2cb1b4cafc"],
}
print("== Paso 2: keys ofuscadas del token E (v2..v5) ==")
hit = 0; tot = 0
for aes, obfs in GT.items():
    for obf in obfs:
        tot += 1
        got = emu.get_aes_key(bytes.fromhex(obf)).hex()
        good = got == aes
        hit += good
        print(f"  {'MATCH' if good else '     '} {obf} -> {got}  esperada {aes}")
print(f"\n  token E: {hit}/{tot} keys revertidas")
if hit:
    print("\n  *** El VM del 483 revierte la ofuscacion del token E. Camino cerrado. ***")
else:
    print("\n  El VM del 483 no revierte el token E: la ofuscacion del E es de otro build.")
