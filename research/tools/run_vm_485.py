"""VM del build 485 (paquete instalado unplayplay 0.0.9) sobre el DLL 485 extraido,
salteando el gate de hash. Valida con los 5 vectores del 485; si pasan, token E.
uso: ppvenv/bin/python run_vm_485.py"""
import hashlib
import sys
from pathlib import Path

SP = Path("/tmp/claude-1000/-desarrollo-git-librespot-cpp/dcb0d1d2-6a97-44f3-92dc-1da719aa851c/scratchpad")
DLL = SP / "Spotify485.dll"

import unplayplay.consts as consts
real = hashlib.sha256(DLL.read_bytes()).digest()
print(f"DLL sha256 : {real.hex()}")
print(f"paquete 485: {consts.SP_CLT_SHA2.hex()}")
print(f"gate       : {'coincide' if real == consts.SP_CLT_SHA2 else 'PARCHEADO (sub-build distinta)'}\n")
consts.SP_CLT_SHA2 = real
for m in list(sys.modules.values()):
    if getattr(m, "SP_CLT_SHA2", None) is not None:
        try: m.SP_CLT_SHA2 = real
        except Exception: pass

from unplayplay import KeyEmu

# 5 vectores publicados del build 485 (token C) -> validan que las VAs aplican
VEC = [
    ("eff09d518b4cad9a8feaf11b1a245870", "a503a84c1dc9271460cc13f142e0bae2"),
    ("e0934811ccbe830d3ea3330bc6a4bd8d", "c3206271b4c70fff8e4ac3993c4dae8a"),
    ("c62a616307aa090a8b718cd9d6a7cd33", "8d86fb522c00729f35b34d60b165b922"),
    ("558bda51efa8c558ad54e87dcea4234a", "0fd3998b706247b3474b2d3cf6d8e31f"),
    ("81f6c97072e83efe106d8f372216ed1b", "4d442c155f9a95258f613a89be957be9"),
]

print("cargando emulador...")
emu = KeyEmu(DLL)

print("\n== vectores del build 485 (validan las VAs) ==")
ok = 0
for obf, want in VEC:
    got = emu.get_aes_key(bytes.fromhex(obf)).hex()
    good = got == want; ok += good
    print(f"  {'OK  ' if good else 'FALLA'} {obf} -> {got}" + ("" if good else f"  (esperada {want})"))
print(f"\n  {ok}/5 vectores reproducidos")
if ok != 5:
    print("  -> Las VAs de esta sub-build no coinciden con el paquete 485. No sirve directo.")
    sys.exit(1)
print("  -> VAs correctas. Este DLL sirve como emulador del 485.\n")

# nuestras keys ofuscadas del token E, con su AES key conocida
GT = {
  "a503a84c1dc9271460cc13f142e0bae2": ["628c9a976bedc0e3663bc9c4051f1708",
      "1a50e35c5335f9f0b108e79b5dc98f8a", "fa699aa7a870569bfc7bd818ed09bab5", "611f9b976f284ecf77c202588b8c1d22"],
  "c3206271b4c70fff8e4ac3993c4dae8a": ["79a7680e258598364d3215881ead3ced",
      "1c94840189ef6e384f5aa185da8a4305", "d247cd8e62215a995e5829b9c7e67f7c", "9678cc71ae205966f814cb500bb7c7cf"],
  "8d86fb522c00729f35b34d60b165b922": ["f4c60dff695fb265104bc50546d6e48a",
      "4c587f8f3b898b63f60ab0043c14a267", "3dfba174e5779b38c5e97d2cb1b4cafc"],
}
print("== keys ofuscadas del token E (v2..v5) ==")
hit = tot = 0
for aes, obfs in GT.items():
    for obf in obfs:
        tot += 1
        got = emu.get_aes_key(bytes.fromhex(obf)).hex()
        good = got == aes; hit += good
        print(f"  {'MATCH' if good else '     '} {obf} -> {got}  esperada {aes}")
print(f"\n  token E: {hit}/{tot} keys revertidas")
print("\n  *** EL VM DEL 485 REVIERTE LA OFUSCACION DEL TOKEN E ***" if hit
      else "\n  el VM del 485 no revierte el token E: es de otro build")
