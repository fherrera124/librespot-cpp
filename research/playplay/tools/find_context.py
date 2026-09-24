import pefile
import capstone

pe = pefile.PE("../playplay/Spotify_1.2.88.485_g1012a6e0.dll", fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase

# let's find references to 0x18178a040 - 0xc00 = 0x181789440 (in 485 subbuild)
target_va = 0x181789440

data = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

# We scan the .text section
for section in pe.sections:
    if b".text" in section.Name:
        start = section.VirtualAddress
        size = section.Misc_VirtualSize
        code = data[start:start+size]
        
        # very slow to disasm all, let's just do a pattern search for RIP-relative addressing?
        pass
