import os
import struct
import binascii
import collections
from unicorn import *
from capstone import *
from unicorn.x86_const import *

DLL_PATH = "../../dlls/Spotify_1.2.92.148_dump.dll"
IMAGE_BASE = 0x180000000

# RVAs
VM_RUNTIME_INIT = 0x49cb88
VM_OBJECT_TRANSFORM = 0x49eaa4
VM_CONSTRUCT_AND_TRANSFORM = 0x4a0268

md = Cs(CS_ARCH_X86, CS_MODE_64)
trace_log = collections.deque(maxlen=20)

HEAP_BASE = 0x300000000
HEAP_PTR = HEAP_BASE
HEAP_SIZE = 10 * 1024 * 1024

def hook_fake_apis(uc, address, size, user_data):
    global HEAP_PTR
    if address == 0x7ffa4974cb40:
        hHeap = uc.reg_read(UC_X86_REG_RCX)
        dwFlags = uc.reg_read(UC_X86_REG_RDX)
        dwBytes = uc.reg_read(UC_X86_REG_R8)
        print(f"\n[API] HeapAlloc(Heap=0x{hHeap:x}, Flags=0x{dwFlags:x}, Size=0x{dwBytes:x})")
        allocated_addr = HEAP_PTR
        HEAP_PTR += (dwBytes + 0xF) & ~0xF # Alineación a 16 bytes
        uc.reg_write(UC_X86_REG_RAX, allocated_addr)
        print(f"      -> Retornando 0x{allocated_addr:x}")
    else:
        # Generic API catch
        rcx = uc.reg_read(UC_X86_REG_RCX)
        rdx = uc.reg_read(UC_X86_REG_RDX)
        r8 = uc.reg_read(UC_X86_REG_R8)
        r9 = uc.reg_read(UC_X86_REG_R9)
        print(f"\n[API] Unknown API called at 0x{address:x}")
        print(f"      RCX=0x{rcx:x} RDX=0x{rdx:x} R8=0x{r8:x} R9=0x{r9:x}")
        
        # Heuristic for memcpy/memmove: RCX and RDX are valid pointers, R8 is size
        if 0 < r8 < 0x10000:
            try:
                # Test if RDX is readable
                src_data = uc.mem_read(rdx, r8)
                # Test if RCX is writable by writing it back
                uc.mem_write(rcx, bytes(src_data))
                print(f"      -> Simulated memcpy(0x{rcx:x}, 0x{rdx:x}, 0x{r8:x})")
            except Exception:
                pass

        # Default return RCX
        uc.reg_write(UC_X86_REG_RAX, rcx)

def hook_code(uc, address, size, user_data):
    try:
        code = uc.mem_read(address, size)
        for i in md.disasm(code, address):
            trace_log.append(f"0x{address:x} (+0x{address - IMAGE_BASE:x}): {i.mnemonic} {i.op_str}")
            break
    except Exception:
        trace_log.append(f"0x{address:x}: [decode error]")

def align_page(addr):
    return (addr + 0xFFF) & ~0xFFF

def hook_mem_unmapped(uc, access, address, size, value, user_data):
    print("\n>>> UNMAPPED MEMORY ACCESS <<<")
    access_type = "UNKNOWN"
    if access == UC_MEM_READ_UNMAPPED:
        access_type = "READ"
    elif access == UC_MEM_WRITE_UNMAPPED:
        access_type = "WRITE"
    elif access == UC_MEM_FETCH_UNMAPPED:
        access_type = "FETCH"
        
    rip = uc.reg_read(UC_X86_REG_RIP)
    try:
        code = uc.mem_read(rip, 15)
        for i in md.disasm(code, rip):
            print(f"Instruction at crash: {i.mnemonic} {i.op_str}")
            break
    except:
        print("Could not disassemble at RIP.")
        
    print(f"[{access_type}] at 0x{address:x} (size {size})")
    print(f"RIP = 0x{rip:x} (Offset in DLL: 0x{rip - IMAGE_BASE:x})")
    
    if access == UC_MEM_FETCH_UNMAPPED:
        page_addr = address & ~0xFFF
        try:
            uc.mem_map(page_addr, 0x1000)
            uc.mem_write(page_addr, b'\xc3' * 0x1000) # fill with RET
            uc.hook_add(UC_HOOK_CODE, hook_fake_apis, begin=page_addr, end=page_addr+0x1000)
            print(f"[+] Dynamically mapped Fake API page at 0x{page_addr:x} with RETs")
            return True
        except Exception as e:
            print(f"[-] Failed to map Fake API memory: {e}")
            return False
        
    # Dynamically map the memory to see how far we can get
    page_addr = address & ~0xFFF
    try:
        uc.mem_map(page_addr, 0x1000)
        print(f"[+] Dynamically mapped 4KB at 0x{page_addr:x}")
        return True # Continue emulation
    except Exception as e:
        print(f"[-] Failed to map memory: {e}")
        return False

def main():
    print(f"[*] Reading DLL from {DLL_PATH}...")
    with open(DLL_PATH, 'rb') as f:
        pe_data = f.read()
        
    print("[*] Initializing Unicorn x64...")
    mu = Uc(UC_ARCH_X86, UC_MODE_64)
    
    # Map DLL
    dll_size_aligned = align_page(len(pe_data))
    print(f"[*] Mapping DLL at 0x{IMAGE_BASE:x} (size: {dll_size_aligned} bytes)")
    mu.mem_map(IMAGE_BASE, dll_size_aligned)
    mu.mem_write(IMAGE_BASE, pe_data)
    
    # Map Stack
    STACK_BASE = 0x100000000
    STACK_SIZE = 2 * 1024 * 1024
    print(f"[*] Mapping Stack at 0x{STACK_BASE:x} (size: {STACK_SIZE} bytes)")
    mu.mem_map(STACK_BASE, STACK_SIZE)
    
    # Map Buffers
    BUFFERS_BASE = 0x200000000
    BUFFERS_SIZE = 1024 * 1024
    print(f"[*] Mapping Buffers at 0x{BUFFERS_BASE:x} (size: {BUFFERS_SIZE} bytes)")
    mu.mem_map(BUFFERS_BASE, BUFFERS_SIZE)

    # Map Heap
    print(f"[*] Mapping Heap at 0x{HEAP_BASE:x} (size: {HEAP_SIZE} bytes)")
    mu.mem_map(HEAP_BASE, HEAP_SIZE)

    # Map TEB
    TEB_BASE = 0x500000000
    print(f"[*] Mapping TEB at 0x{TEB_BASE:x}")
    mu.mem_map(TEB_BASE, 0x1000)
    mu.reg_write(UC_X86_REG_GS_BASE, TEB_BASE)
    
    # PEB Pointer at TEB+0x60
    PEB_BASE = 0x500010000
    mu.mem_map(PEB_BASE, 0x1000)
    mu.mem_write(TEB_BASE + 0x60, struct.pack('<Q', PEB_BASE))
    mu.mem_write(TEB_BASE + 0x30, struct.pack('<Q', TEB_BASE)) # LinearAddress of TEB

    # Map Fake API space
    FAKE_API_BASE = 0x7ffa4974c000
    print(f"[*] Mapping Fake API area at 0x{FAKE_API_BASE:x}")
    mu.mem_map(FAKE_API_BASE, 0x1000)
    mu.mem_write(FAKE_API_BASE, b'\xc3' * 0x1000)
    
    # Hook Fake API (solo para este rango)
    mu.hook_add(UC_HOOK_CODE, hook_fake_apis, begin=FAKE_API_BASE, end=FAKE_API_BASE + 0x1000)
    
    # Allocate buffers
    ptr_vm_obj = BUFFERS_BASE + 0x100
    ptr_obf = BUFFERS_BASE + 0x200
    ptr_derived = BUFFERS_BASE + 0x300
    ptr_init = BUFFERS_BASE + 0x400
    
    # Write inputs
    obf = bytes.fromhex("628c9a976bedc0e3663bc9c4051f1708")
    init_val = bytes.fromhex("a33c929c31c3e0eefeedebdaa1e7ff7f")
    
    mu.mem_write(ptr_obf, obf)
    mu.mem_write(ptr_init, init_val)
    
    # Setup stack & return address
    rsp = (STACK_BASE + STACK_SIZE - 0x1000) & ~0xF
    rsp -= 8
    RET_ADDR = 0xDEADBEEF
    mu.mem_map(0xDEADB000, 0x1000) # Dummy map for ret addr
    mu.mem_write(rsp, struct.pack('<Q', RET_ADDR))
    
    mu.reg_write(UC_X86_REG_RSP, rsp)
    
    # Hook missing memory and code execution
    mu.hook_add(UC_HOOK_MEM_UNMAPPED, hook_mem_unmapped)
    mu.hook_add(UC_HOOK_CODE, hook_code)
    
    # We should call VmRuntimeInit just in case!
    print("[*] Setting up registers for VmConstructAndTransform...")
    mu.reg_write(UC_X86_REG_RCX, ptr_vm_obj)
    mu.reg_write(UC_X86_REG_RDX, ptr_obf)
    mu.reg_write(UC_X86_REG_R8, ptr_derived)
    mu.reg_write(UC_X86_REG_R9, ptr_init)
    
    target_func = IMAGE_BASE + VM_CONSTRUCT_AND_TRANSFORM
    print(f"[*] Starting emulation at 0x{target_func:x}")
    try:
        mu.emu_start(target_func, RET_ADDR)
        print("[+] Emulation finished without crash.")
    except UcError as e:
        print(f"\n[!] Emulation stopped: {e}")
        rip = mu.reg_read(UC_X86_REG_RIP)
        print(f"[!] Final RIP = 0x{rip:x} (offset: 0x{rip - IMAGE_BASE:x})")
        print("\n--- Last 20 instructions ---")
        for t in trace_log:
            print(t)

    derived_out = mu.mem_read(ptr_derived, 28)
    print(f"\n[*] Derived key buf: {bytes(derived_out).hex()}")

if __name__ == '__main__':
    main()
