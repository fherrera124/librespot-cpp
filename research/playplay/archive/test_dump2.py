import ctypes
import sys

kernel32 = ctypes.windll.kernel32
kernel32.VirtualAlloc.restype = ctypes.c_void_p
kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]

PAGE_EXECUTE_READWRITE = 0x40
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000

print("Reading dump...")
with open("Spotify_dump_new.dll", "rb") as f:
    data = f.read()

size = len(data)
with open("base_addr.txt", "r") as f:
    desired_base = int(f.read().strip())

base_addr = kernel32.VirtualAlloc(desired_base, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
if not base_addr:
    print(f"VirtualAlloc failed for base {hex(desired_base)}, trying anywhere...")
    base_addr = kernel32.VirtualAlloc(None, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    if not base_addr:
        print("VirtualAlloc failed completely")
        sys.exit(1)

print(f"Allocated at {hex(base_addr)}, copying memory...")
ctypes.memmove(base_addr, data, size)

VmRuntimeInit = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int)
VmObjectTransform = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)

print("Resolving RVAs...")
init_func = VmRuntimeInit(base_addr + 0x4b35f8)
transform_func = VmObjectTransform(base_addr + 0x4b5538)

vm_obj = ctypes.create_string_buffer(144)
vm_rt_context = ctypes.create_string_buffer(16)
ctypes.memset(vm_rt_context, 0, 16)

print("Calling init_func...")
sys.stdout.flush()
try:
    init_func(ctypes.addressof(vm_obj), ctypes.addressof(vm_rt_context), 1)
    print("init_func success!")
except Exception as e:
    print(f"Init exception: {e}")

sys.stdout.flush()

obf_buf = ctypes.create_string_buffer(bytes.fromhex("628c9a976bedc0e3663bc9c4051f1708"))
init_val_buf = ctypes.create_string_buffer(bytes.fromhex("8df84f8c610a1ab4c449a214fb08305e"))
derived_buf = ctypes.create_string_buffer(24)

print("Calling transform_func...")
sys.stdout.flush()
try:
    transform_func(ctypes.addressof(vm_obj), ctypes.addressof(obf_buf), ctypes.addressof(derived_buf), ctypes.addressof(init_val_buf))
    print("transform_func success!")
except Exception as e:
    print(f"Transform exception: {e}")

sys.stdout.flush()
expected_aes = bytes.fromhex("a503a84c1dc9271460cc13f142e0bae2")

if expected_aes in derived_buf.raw:
    print(f"AES in derived_buf offset {derived_buf.raw.index(expected_aes)}")
elif expected_aes in vm_obj.raw:
    print(f"AES in vm_obj offset {vm_obj.raw.index(expected_aes)}")
else:
    print("AES not found!")
