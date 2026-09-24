import capstone
import yara
import binascii

def get_bytes(filename, rva, size):
    with open(filename, "rb") as f:
        # Assuming RVA is file offset for dumped DLLs.
        # Wait, if it's a dumped DLL, RVA == File Offset!
        # But wait! For 485, is it a dumped DLL?
        f.seek(rva)
        return f.read(size)

# For a normal DLL on disk, we must convert RVA to File Offset!
import pefile
pe_485 = pefile.PE("../playplay/Spotify_1.2.88.485_g1012a6e0.dll", fast_load=True)

def rva_to_offset(pe, rva):
    for section in pe.sections:
        if section.VirtualAddress <= rva < section.VirtualAddress + section.SizeOfRawData:
            return rva - section.VirtualAddress + section.PointerToRawData
    return rva # fallback if dumped

def get_bytes_pe(pe, filename, rva, size):
    offset = rva_to_offset(pe, rva)
    with open(filename, "rb") as f:
        f.seek(offset)
        return f.read(size)

vas_485 = {
    "MTX_LOCK_VA": 0x1644980,
    "MTX_UNLOCK_VA": 0x16449AC,
    "MALLOC_VA": 0x166FFF0,
    "FILL_RANDOM_BYTES_VA": 0x432BEC,
    "CXX_THROW_EXCEPTION_VA": 0x16603A8,
}

pe_667 = pefile.PE("../playplay/Spotify_1.2.92.148_dump.dll", fast_load=True)

for name, rva in vas_485.items():
    code_bytes = get_bytes_pe(pe_485, "../playplay/Spotify_1.2.88.485_g1012a6e0.dll", rva, 32)
    # create yara rule with first 16 bytes
    pattern = " ".join([f"{b:02x}" for b in code_bytes[:16]])
    rule = f"""
    rule {name} {{
        strings:
            $a = {{ {pattern} }}
        condition:
            $a
    }}
    """
    try:
        compiled = yara.compile(source=rule)
        matches = compiled.match("../playplay/Spotify_1.2.92.148_dump.dll")
        for m in matches:
            for s in m.strings:
                for inst in s.instances:
                    # inst.offset is File Offset. Convert to RVA!
                    # For dumped DLL, offset == RVA. But for our 667, is it dumped?
                    # Let's just print file offset and we can convert manually.
                    print(f"{name} found at file offset: {hex(inst.offset)}")
    except Exception as e:
        print(f"Error for {name}: {e}")

