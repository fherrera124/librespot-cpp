import os
import sys
import ctypes
import binascii




# Known vector for Token E (v2) from ground-truth-vectors.json
# File ID: 2f43127d80edc9cd9f12f441e1cb7904b680f9da
KNOWN_OBF = bytes.fromhex("628c9a976bedc0e3663bc9c4051f1708")
KNOWN_AES = bytes.fromhex("a503a84c1dc9271460cc13f142e0bae2")
INIT_VALUE = bytes.fromhex("8df84f8c610a1ab4c449a214fb08305e")

class PlayPlayService:
    def __init__(self, dll_path):
        if not os.path.exists(dll_path):
            raise FileNotFoundError(f"DLL not found: {dll_path}")
        
        print(f"Loading {dll_path}...")
        self.dll = ctypes.WinDLL(dll_path)
        
        # RVAs for 1.2.93.667
        # Found via YARA signatures matching the 485 subbuild
        self.VM_RUNTIME_INIT_RVA = 0x4b35f8
        self.VM_OBJECT_TRANSFORM_RVA = 0x4b5538
        
        # We need the ImageBase of the loaded DLL
        self.base_addr = ctypes.cast(self.dll._handle, ctypes.c_void_p).value
        print(f"DLL loaded at: {hex(self.base_addr)}")
        
        self.vm_runtime_init = ctypes.cast(self.base_addr + self.VM_RUNTIME_INIT_RVA, ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int))
        self.vm_object_transform = ctypes.cast(self.base_addr + self.VM_OBJECT_TRANSFORM_RVA, ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p))
        
        # Allocate buffers
        self.vm_obj = ctypes.create_string_buffer(144)
        self.vm_rt_context = ctypes.create_string_buffer(16)
        self.obf_buf = ctypes.create_string_buffer(16)
        self.derived_buf = ctypes.create_string_buffer(24)
        self.init_val_buf = ctypes.create_string_buffer(INIT_VALUE)
        
        # We don't have the exact RUNTIME_CONTEXT_VA, but since we run natively, 
        # we can just pass a dummy one or scan for it if it crashes.
        # Often the DLL handles it if it's zeroed out, or we might not even need it.
        ctypes.memset(self.vm_rt_context, 0, 16)
        
        print("Initializing VM Runtime...")
        try:
            self.vm_runtime_init(self.vm_obj, self.vm_rt_context, 1)
        except Exception as e:
            print(f"VM Init crashed (expected if context is strict): {e}")
            # If it crashes, we might need a better context. But let's proceed.

        # Auto-discover AES key offset
        print("Auto-discovering AES key output location...")
        self.obf_buf.value = KNOWN_OBF
        
        self.vm_object_transform(self.vm_obj, self.obf_buf, self.derived_buf, self.init_val_buf)
        
        # Scan derived_buf, vm_obj, and stack for the AES key
        if KNOWN_AES in self.derived_buf.raw:
            self.aes_offset = self.derived_buf.raw.index(KNOWN_AES)
            self.aes_location = "derived_buf"
            print(f"AES key found in derived_buf at offset {self.aes_offset}")
        elif KNOWN_AES in self.vm_obj.raw:
            self.aes_offset = self.vm_obj.raw.index(KNOWN_AES)
            self.aes_location = "vm_obj"
            print(f"AES key found in vm_obj at offset {self.aes_offset}")
        else:
            print("AES key not found in primary buffers! The key might be on the thread stack or requires TRIGGER_RIP hook.")
            self.aes_location = None

    def deobfuscate(self, obf_key_hex):
        obf_bytes = bytes.fromhex(obf_key_hex)
        ctypes.memmove(self.obf_buf, obf_bytes, 16)
        
        self.vm_object_transform(self.vm_obj, self.obf_buf, self.derived_buf, self.init_val_buf)
        
        if self.aes_location == "derived_buf":
            return self.derived_buf.raw[self.aes_offset:self.aes_offset+16].hex()
        elif self.aes_location == "vm_obj":
            return self.vm_obj.raw[self.aes_offset:self.aes_offset+16].hex()
        return None

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python lan_deobfuscator.py <path_to_Spotify.dll>")
        sys.exit(1)
        
    dll_path = sys.argv[1]
    if os.name != 'nt':
        print("This script must be run on Windows (Vía A).")
        sys.exit(1)
        
    try:
        service = PlayPlayService(dll_path)
        print("Test extraction: ", service.deobfuscate(KNOWN_OBF.hex()))
    except Exception as e:
        print(f"Error: {e}")
