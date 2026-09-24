import logging
from unplayplay.emu_session import EmuSession
from unplayplay.emu.heap_allocator import HeapAllocator
from unplayplay.emu import runtime
from unplayplay.consts import EMULATOR_SIZES, MEM
import unicorn
import pefile

logging.basicConfig(level=logging.INFO)

pe = pefile.PE("../playplay/Spotify_1.2.92.148_dump.dll", fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase
image_size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF

mu = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_64)
mu.mem_map(image_base, image_size)
with open("../playplay/Spotify_1.2.92.148_dump.dll", "rb") as f:
    mu.mem_write(image_base, f.read())

heap = HeapAllocator.create(mu, MEM.HEAP_ADDR, MEM.HEAP_SIZE)
runtime.setup_stack(mu)
runtime.setup_teb(mu)

vm_obj = heap.alloc(EMULATOR_SIZES.VM_OBJECT)
vm_rt_context = heap.alloc(EMULATOR_SIZES.RT_CONTEXT)
vm_init_value = heap.alloc(EMULATOR_SIZES.INIT_VALUE)
obfuscated_key = heap.alloc(EMULATOR_SIZES.OBFUSCATED_KEY)
derived_key = heap.alloc(EMULATOR_SIZES.DERIVED_KEY)

def hook_unmapped(mu, type, address, size, value, user_data):
    rip = mu.reg_read(unicorn.x86_const.UC_X86_REG_RIP)
    print(f">>> UNMAPPED ACCESS: type={type} addr=0x{address:X} RIP=0x{rip:X}")
    return False

mu.hook_add(unicorn.UC_HOOK_MEM_UNMAPPED, hook_unmapped)

print("Running VM_RUNTIME_INIT...")
try:
    runtime.emulate_call(mu, image_base + 0x49CB88, [vm_obj.ptr, vm_rt_context.ptr, 1])
    print("Init done.")
except Exception as e:
    print(f"Init exception: {e}")

obf = bytes.fromhex("628c9a976bedc0e3663bc9c4051f1708")
obfuscated_key.write(obf)

print("Running VM_OBJECT_TRANSFORM...")
try:
    runtime.emulate_call(mu, image_base + 0x49EAA4, [vm_obj.ptr, obfuscated_key.ptr, derived_key.ptr, vm_init_value.ptr])
    print("Transform done.")
except Exception as e:
    print(f"Transform exception: {e}")

