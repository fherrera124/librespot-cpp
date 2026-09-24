"""Corre el VM del build 483 sobre el Spotify.dll extraido (sub-build en
../playplay/), salteando el gate de sha256.

Paso 1 - VALIDACION: reproduce los 5 vectores publicados del build 483. Si pasan,
         las VAs de este DLL coinciden y el emulador es correcto.
Paso 2 - si el paso 1 pasa: prueba los 11 vectores del token E (v2..v5).

OJO: usa el CODIGO del build 483 de another-unplayplay (no el 485 del paquete
instalado). Hay que tener ese source; seteá UPP483_SRC a
.../another-unplayplay-<commit>/src . La sub-build de ../playplay/ tiene VAs
distintas -> hoy FALLA (UC_ERR_FETCH_UNMAPPED); sirve con el DLL 483 CANONICO
(sha256 9cafe1ca...) o tras reversear. Ver PLAN.md.

uso: UPP483_SRC=/ruta/src ppvenv/bin/python run_vm_483.py [ruta/al/Spotify.dll]
"""
import hashlib
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_DLL = HERE.parent.parent / "playplay" / "Spotify_1.2.88.483_g8aa8628e.dll"
DLL = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DLL

SRC = os.environ.get("UPP483_SRC")  # .../another-unplayplay-<commit>/src (build 483)
if SRC:
    sys.path.insert(0, SRC)

import unplayplay.consts as consts
real_sha = hashlib.sha256(DLL.read_bytes()).digest()
print(f"DLL          : {DLL}")
print(f"DLL sha256   : {real_sha.hex()}")
print(f"paquete 483  : {consts.SP_CLT_SHA2.hex()}")
print(f"gate         : {'coincide' if real_sha == consts.SP_CLT_SHA2 else 'PARCHEADO (difiere)'}\n")
consts.SP_CLT_SHA2 = real_sha
for m in list(sys.modules.values()):
    if getattr(m, "SP_CLT_SHA2", None) is not None:
        try: m.SP_CLT_SHA2 = real_sha
        except Exception: pass

from unplayplay import KeyEmu

# --- Paso 1: vectores publicados del build 483 (token F) ---
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
    good = got == want; ok += good
    print(f"  {'OK  ' if good else 'FALLA'} {obf} -> {got}" + ("" if good else f"  (esperada {want})"))
print(f"\n  {ok}/{len(VEC_483)} vectores del 483 reproducidos")
if ok != len(VEC_483):
    print("\n  -> Las VAs de este DLL NO coinciden con el paquete 483 (otra sub-build).")
    print("     Hace falta el DLL sha256 9cafe1ca..., o probar el build 485/667.")
    sys.exit(1)
print("  -> VAs correctas. Este DLL sirve como emulador del 483.\n")

# --- Paso 2: keys ofuscadas del token E ---
GT = {
  "a503a84c1dc9271460cc13f142e0bae2": ["628c9a976bedc0e3663bc9c4051f1708",
      "1a50e35c5335f9f0b108e79b5dc98f8a","fa699aa7a870569bfc7bd818ed09bab5","611f9b976f284ecf77c202588b8c1d22"],
  "c3206271b4c70fff8e4ac3993c4dae8a": ["79a7680e258598364d3215881ead3ced",
      "1c94840189ef6e384f5aa185da8a4305","d247cd8e62215a995e5829b9c7e67f7c","9678cc71ae205966f814cb500bb7c7cf"],
  "8d86fb522c00729f35b34d60b165b922": ["f4c60dff695fb265104bc50546d6e48a",
      "4c587f8f3b898b63f60ab0043c14a267","3dfba174e5779b38c5e97d2cb1b4cafc"],
}
print("== Paso 2: keys ofuscadas del token E (v2..v5) ==")
hit = tot = 0
for aes, obfs in GT.items():
    for obf in obfs:
        tot += 1
        got = emu.get_aes_key(bytes.fromhex(obf)).hex()
        good = got == aes; hit += good
        print(f"  {'MATCH' if good else '     '} {obf} -> {got}  esperada {aes}")
print(f"\n  token E: {hit}/{tot} keys revertidas")
if hit:
    print("\n  *** El VM del 483 revierte la ofuscacion del token E. ***")
else:
    print("\n  El VM del 483 no revierte el token E: la ofuscacion del E es de otro build.")
