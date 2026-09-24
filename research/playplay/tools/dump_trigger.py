import pefile
import capstone

pe = pefile.PE("../playplay/Spotify_1.2.88.485_g1012a6e0.dll", fast_load=True)
data = pe.get_memory_mapped_image()
trigger_rva = 0x180411de0 - pe.OPTIONAL_HEADER.ImageBase

code = data[trigger_rva:trigger_rva+64]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i in md.disasm(code, 0x180411de0):
    print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}\t\t{i.bytes.hex()}")

