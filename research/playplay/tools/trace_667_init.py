import pefile
import capstone

pe = pefile.PE("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll", fast_load=True)
data = pe.get_memory_mapped_image()
image_base = pe.OPTIONAL_HEADER.ImageBase
rva = pe.get_rva_from_offset(0x4b29f8)

code = data[rva:rva+0x60]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
for i in md.disasm(code, image_base + rva):
    print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")

