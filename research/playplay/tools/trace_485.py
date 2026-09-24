import logging
from pathlib import Path
from unplayplay.key_emu import KeyEmu
import unicorn
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

logging.basicConfig(level=logging.INFO)

md = Cs(CS_ARCH_X86, CS_MODE_64)

def hook_code(mu, address, size, user_data):
    mem = mu.mem_read(address, size)
    for i in md.disasm(mem, address):
        print(f"0x{i.address:X}:\t{i.mnemonic}\t{i.op_str}")

def hook_unmapped(mu, type, address, size, value, user_data):
    print(f">>> UNMAPPED ACCESS: addr=0x{address:X} RIP=0x{mu.reg_read(unicorn.x86_const.UC_X86_REG_RIP):X}")
    return False

original_init = KeyEmu._init_runtime
def my_init_runtime(self, mu, vm_obj, vm_rt_context):
    mu.hook_add(unicorn.UC_HOOK_CODE, hook_code)
    mu.hook_add(unicorn.UC_HOOK_MEM_UNMAPPED, hook_unmapped)
    original_init(self, mu, vm_obj, vm_rt_context)
    
KeyEmu._init_runtime = my_init_runtime

emu = KeyEmu(Path("../playplay/Spotify_1.2.88.485_g1012a6e0.dll"))
obf = bytes.fromhex("628c9a976bedc0e3663bc9c4051f1708")

try:
    emu.get_aes_key(obf)
except Exception as e:
    pass
