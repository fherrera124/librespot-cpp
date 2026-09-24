import hashlib
import sys
from pathlib import Path
import unplayplay.consts as consts

DLL = Path("../playplay/Spotify_1.2.88.485_g1012a6e0.dll")
# shift everything by -0xC00
consts.RT_FUNCTIONS.VM_RUNTIME_INIT_VA -= 0xc00
consts.RT_FUNCTIONS.VM_OBJECT_TRANSFORM_VA -= 0xc00
consts.AES_KEY_HOOK.TRIGGER_RIP -= 0xc00
consts.RT_FUNCTIONS.CXX_THROW_EXCEPTION_VA -= 0xc00
consts.RT_DATA.RUNTIME_CONTEXT_VA -= 0xc00
consts.RT_HOOKS.MTX_LOCK_VA -= 0xc00
consts.RT_HOOKS.CND_WAIT_VA -= 0xc00
consts.RT_HOOKS.MTX_UNLOCK_VA -= 0xc00
consts.RT_HOOKS.MALLOC_VA -= 0xc00
consts.VM_HOOKS.FILL_RANDOM_BYTES_VA -= 0xc00

from unplayplay import KeyEmu
emu = KeyEmu(DLL)

obf = "eff09d518b4cad9a8feaf11b1a245870"
session = emu._create_session()
session.obfuscated_key.write(bytes.fromhex(obf))
import unplayplay.emu.runtime as runtime

# We can patch TRIGGER_RIP to NOT stop emulation, or just let it stop
try:
    runtime.emulate_call(session.mu, emu._vm_object_transform, [session.vm_obj.ptr, session.obfuscated_key.ptr, session.derived_key.ptr, session.init_value.ptr])
except Exception as e:
    pass

print(f"derived_key buffer: {session.derived_key.read().hex()}")
print(f"captured_aes_key: {session.captured_aes_key.hex() if session.captured_aes_key else 'None'}")
