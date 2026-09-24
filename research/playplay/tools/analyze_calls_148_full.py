import capstone
import sys

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
with open("../playplay/Spotify_1.2.92.148_dump.dll", "rb") as f:
    data = f.read()

image_base = 0x180000000

def disassemble_func(rva, name):
    print(f"\n--- {name} ---")
    func_data = data[rva:rva+0x2000]
    for i in md.disasm(func_data, image_base + rva):
        if i.mnemonic == "call":
            print(f"0x{i.address:X}:\t{i.mnemonic}\t{i.op_str}")
        if i.mnemonic == "ret":
            print(f"0x{i.address:X}:\t{i.mnemonic}")
            break

disassemble_func(0x49CB88, "VM_RUNTIME_INIT")
disassemble_func(0x49EAA4, "VM_OBJECT_TRANSFORM")
