import yara
import pefile

# The trigger RIP in 485 is:
# 0x0: call 0x1297d90  (e8 8b 7d 29 01)
# 0x5: imul r9d, r12d, 0x76738fb (45 69 cc fb 38 67 07)
# 0xc: mov eax, r12d (41 8b c4)
# 0xf: add r15, -0x80 (49 83 c7 80)
# 0x13: and al, 0x10 (24 10)
# 0x15: shr r9d, 0x19 (41 c1 e9 19)

rules = yara.compile(source="""
rule trigger_rip {
    strings:
        $ = { 45 69 cc fb 38 67 07 41 8b c4 49 83 c7 80 24 10 41 c1 e9 19 f6 d8 1b c9 81 e1 77 3f 03 24 81 c1 }
    condition:
        all of them
}
""")
pe = pefile.PE("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll", fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase
for match in rules.match("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll"):
    for string_match in match.strings:
        for instance in string_match.instances:
            # We matched starting from offset 0x5 (after the call). 
            # So the call is at instance.offset - 5
            va = image_base + instance.offset - 5
            print(f"TRIGGER_RIP Offset: {hex(instance.offset - 5)}, VA: {hex(va)}")
