import pefile

pe = pefile.PE("../playplay/Spotify_1.2.88.485_g1012a6e0.dll", fast_load=True)
data = pe.get_memory_mapped_image()
image_base = pe.OPTIONAL_HEADER.ImageBase

# 485 subbuild shifted by -0xc00
ctx_rva = 0x18178a040 - 0xc00 - image_base
print(f"Bytes at ctx_rva: {data[ctx_rva:ctx_rva+64].hex()}")

