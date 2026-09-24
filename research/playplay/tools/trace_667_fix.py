import pefile
import capstone

pe = pefile.PE("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll", fast_load=True)
data = pe.get_memory_mapped_image()
image_base = pe.OPTIONAL_HEADER.ImageBase

rva_init = pe.get_rva_from_offset(0x4b29f8)
rva_transform = pe.get_rva_from_offset(0x4b4938)

print(f"VM_RUNTIME_INIT RVA: {hex(rva_init)}, VA: {hex(image_base + rva_init)}")
print(f"VM_OBJECT_TRANSFORM RVA: {hex(rva_transform)}, VA: {hex(image_base + rva_transform)}")
