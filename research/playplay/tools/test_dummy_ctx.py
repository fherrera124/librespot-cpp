import hashlib
import sys
from pathlib import Path
import unplayplay.consts as consts

# 485 subbuild
DLL = Path("../playplay/Spotify_1.2.88.485_g1012a6e0.dll")
real = hashlib.sha256(DLL.read_bytes()).digest()
consts.SP_CLT_SHA2 = real
for m in list(sys.modules.values()):
    if getattr(m, "SP_CLT_SHA2", None) is not None:
        try: m.SP_CLT_SHA2 = real
        except: pass

# Patch RVAs for 485 subbuild!
consts.RT_FUNCTIONS.VM_RUNTIME_INIT_VA = 0x1803e42ac
consts.RT_FUNCTIONS.VM_OBJECT_TRANSFORM_VA = 0x1803e6398
consts.AES_KEY_HOOK.TRIGGER_RIP = 0x1804129e0
consts.RT_FUNCTIONS.CXX_THROW_EXCEPTION_VA = 0x1816603a8

# Try with a DUMMY RUNTIME_CONTEXT_VA!
consts.RT_DATA.RUNTIME_CONTEXT_VA = 0x181000000 # Dummy!

from unplayplay import KeyEmu
emu = KeyEmu(DLL)

# known token C vector for 485
obf = "eff09d518b4cad9a8feaf11b1a245870"
want = "a503a84c1dc9271460cc13f142e0bae2"
got = emu.get_aes_key(bytes.fromhex(obf)).hex()
print(f"got: {got}, want: {want}")
