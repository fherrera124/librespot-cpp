import pefile
import capstone

pe = pefile.PE("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll", fast_load=True)
data = pe.get_memory_mapped_image()
image_base = pe.OPTIONAL_HEADER.ImageBase

transform_rva = 0x1804b4938 - image_base

# The AES key extraction happens somewhere inside or called by VM_OBJECT_TRANSFORM.
# Actually, TRIGGER_RIP is in a different function called by VM_OBJECT_TRANSFORM!
# Let's just disassemble VM_OBJECT_TRANSFORM and find the calls.
code = data[transform_rva:transform_rva+0x200]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
for i in md.disasm(code, image_base + transform_rva):
    print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")

