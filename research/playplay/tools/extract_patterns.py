import sys
import pefile
from pathlib import Path

def get_bytes(pe, rva, size=64):
    try:
        return pe.get_memory_mapped_image()[rva:rva+size]
    except Exception as e:
        print(f"Error reading RVA {hex(rva)}: {e}")
        return None

def main():
    dll_485_path = "../playplay/Spotify_1.2.88.485_g1012a6e0.dll"
    
    pe_485 = pefile.PE(dll_485_path, fast_load=True)
    
    # 485 VAs from consts.py (ImageBase is 0x180000000)
    image_base = 0x180000000
    vm_runtime_init_va = 0x00000001803E42AC
    vm_object_transform_va = 0x00000001803E6398
    trigger_rip = 0x00000001804129E0
    
    print("485 Patterns:")
    print("VM_RUNTIME_INIT:", get_bytes(pe_485, vm_runtime_init_va - image_base).hex())
    print("VM_OBJECT_TRANSFORM:", get_bytes(pe_485, vm_object_transform_va - image_base).hex())
    print("TRIGGER_RIP:", get_bytes(pe_485, trigger_rip - image_base).hex())

if __name__ == '__main__':
    main()

