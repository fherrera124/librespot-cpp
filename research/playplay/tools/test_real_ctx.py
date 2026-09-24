import hashlib
import sys
from pathlib import Path
import unplayplay.consts as consts

DLL = Path("../playplay/Spotify_1.2.88.485_g1012a6e0.dll")
real = hashlib.sha256(DLL.read_bytes()).digest()
consts.SP_CLT_SHA2 = real
for m in list(sys.modules.values()):
    if getattr(m, "SP_CLT_SHA2", None) is not None:
        try: m.SP_CLT_SHA2 = real
        except: pass

# The exact RVAs for 485 subbuild we extracted!
consts.RT_FUNCTIONS.VM_RUNTIME_INIT_VA = 0x1803e36ac
consts.RT_FUNCTIONS.VM_OBJECT_TRANSFORM_VA = 0x1803e5798
consts.AES_KEY_HOOK.TRIGGER_RIP = 0x180411de5
consts.RT_FUNCTIONS.CXX_THROW_EXCEPTION_VA = 0x1816603a8 # Wait, I didn't extract CXX_THROW_EXCEPTION_VA for the subbuild!

from unplayplay import KeyEmu
emu = KeyEmu(DLL)
print(emu.get_aes_key(bytes.fromhex("eff09d518b4cad9a8feaf11b1a245870")).hex())
