import ctypes
import struct
import sys
import pefile

PAGE_EXECUTE_READWRITE = 0x40
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000

kernel32 = ctypes.windll.kernel32
kernel32.VirtualAlloc.restype = ctypes.c_void_p
kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
kernel32.RtlAddFunctionTable.restype = ctypes.c_bool
kernel32.RtlAddFunctionTable.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64]

def map_pe(dll_path):
    pe = pefile.PE(dll_path)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    size_of_image = pe.OPTIONAL_HEADER.SizeOfImage
    
    base_addr = kernel32.VirtualAlloc(image_base, size_of_image, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    if not base_addr:
        base_addr = kernel32.VirtualAlloc(None, size_of_image, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    
    print(f"Allocated at {hex(base_addr)}", flush=True)
    
    with open(dll_path, 'rb') as f:
        headers = f.read(pe.OPTIONAL_HEADER.SizeOfHeaders)
        ctypes.memmove(base_addr, headers, len(headers))
        
    for section in pe.sections:
        if section.SizeOfRawData > 0:
            with open(dll_path, 'rb') as f:
                f.seek(section.PointerToRawData)
                data = f.read(section.SizeOfRawData)
                ctypes.memmove(base_addr + section.VirtualAddress, data, len(data))
                
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll_name = entry.dll.decode('ascii')
        print(f"Loading {dll_name}...", flush=True)
        h_dll = kernel32.LoadLibraryA(entry.dll)
        
        for imp in entry.imports:
            if imp.name:
                func_ptr = kernel32.GetProcAddress(h_dll, imp.name)
            else:
                func_ptr = kernel32.GetProcAddress(h_dll, imp.ordinal)
                
            if func_ptr:
                struct.pack_into('<Q', (ctypes.c_char * size_of_image).from_address(base_addr), imp.address - image_base, func_ptr)
                
    if base_addr != image_base and hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
        delta = base_addr - image_base
        for reloc in pe.DIRECTORY_ENTRY_BASERELOC:
            for entry in reloc.entries:
                if entry.type == pefile.RELOCATION_TYPE['IMAGE_REL_BASED_DIR64']:
                    val = struct.unpack_from('<Q', (ctypes.c_char * size_of_image).from_address(base_addr), entry.rva)[0]
                    struct.pack_into('<Q', (ctypes.c_char * size_of_image).from_address(base_addr), entry.rva, val + delta)
                    
    # Register SEH Table (.pdata)
    try:
        pdata_dir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3] # IMAGE_DIRECTORY_ENTRY_EXCEPTION
        if pdata_dir.VirtualAddress > 0 and pdata_dir.Size > 0:
            pdata_rva = pdata_dir.VirtualAddress
            pdata_size = pdata_dir.Size
            entry_count = pdata_size // 12
            print(f"Registering SEH table at RVA {hex(pdata_rva)} with {entry_count} entries...", flush=True)
            if kernel32.RtlAddFunctionTable(base_addr + pdata_rva, entry_count, base_addr):
                print("SEH table registered.", flush=True)
            else:
                print("SEH table registration failed.", flush=True)
    except Exception as e:
        print(f"Exception while registering SEH: {e}", flush=True)

    if hasattr(pe, 'DIRECTORY_ENTRY_TLS') and pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks:
        cb_addr = pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks - image_base
        print(f"Executing TLS Callbacks at RVA {hex(cb_addr)}...", flush=True)
        
        offset = 0
        while True:
            cb_ptr = struct.unpack_from('<Q', (ctypes.c_char * size_of_image).from_address(base_addr), cb_addr + offset)[0]
            if not cb_ptr:
                break
                
            if base_addr != image_base:
                cb_ptr += (base_addr - image_base)
                
            print(f"  -> Calling TLS Callback at {hex(cb_ptr)}...", flush=True)
            TlsCallbackType = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)
            cb = TlsCallbackType(cb_ptr)
            try:
                cb(base_addr, 1, 0)
                print("  -> TLS Callback finished.", flush=True)
            except Exception as e:
                print(f"  -> TLS Callback crashed: {e}", flush=True)
                
            offset += 8
            
    return base_addr, pe.OPTIONAL_HEADER.AddressOfEntryPoint

if __name__ == "__main__":
    dll_path = sys.argv[1]
    
    base_addr, entry_point = map_pe(dll_path)
    
    # Skip DllMain as it causes access violations in a python process
    # DllMainType = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)
    # dll_main = DllMainType(base_addr + entry_point)
    # try:
    #     res = dll_main(base_addr, 1, 0)
    # except Exception as e:
    #     pass
        
    VmRuntimeInit = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int)
    VmObjectTransform = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
    
    init_func = VmRuntimeInit(base_addr + 0x4b35f8)
    transform_func = VmObjectTransform(base_addr + 0x4b5538)
    
    vm_obj = ctypes.create_string_buffer(144)
    vm_rt_context = ctypes.create_string_buffer(16)
    ctypes.memset(vm_rt_context, 0, 16)
    
    print("Calling init...", flush=True)
    try:
        init_func(ctypes.addressof(vm_obj), ctypes.addressof(vm_rt_context), 1)
        print("Init success.", flush=True)
    except Exception as e:
        print(f"Init exception: {e}", flush=True)
        
    obf_buf = ctypes.create_string_buffer(bytes.fromhex("628c9a976bedc0e3663bc9c4051f1708"))
    init_val_buf = ctypes.create_string_buffer(bytes.fromhex("8df84f8c610a1ab4c449a214fb08305e"))
    derived_buf = ctypes.create_string_buffer(24)
    
    print("Calling transform...", flush=True)
    try:
        transform_func(ctypes.addressof(vm_obj), ctypes.addressof(obf_buf), ctypes.addressof(derived_buf), ctypes.addressof(init_val_buf))
        print("Transform success.", flush=True)
    except Exception as e:
        print(f"Transform exception: {e}", flush=True)
        
    expected_aes = bytes.fromhex("a503a84c1dc9271460cc13f142e0bae2")
    
    if expected_aes in derived_buf.raw:
        print(f"Found AES in derived_buf at offset {derived_buf.raw.index(expected_aes)}", flush=True)
    elif expected_aes in vm_obj.raw:
        print(f"Found AES in vm_obj at offset {vm_obj.raw.index(expected_aes)}", flush=True)
    else:
        print("AES not found!", flush=True)
        print("derived_buf:", derived_buf.raw.hex())
        print("vm_obj[:32]:", vm_obj.raw[:32].hex())
