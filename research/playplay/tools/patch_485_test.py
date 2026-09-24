import yara
import pefile

rules = yara.compile(source="""
rule vm_runtime_init {
    strings:
        $ = { 40 53 48 83 ec 20 48 8b d9 45 85 c0 74 ?? 48 8d 05 ?? ?? ?? ?? 48 89 01 48 8d 05 ?? ?? ?? ?? 48 89 41 18 }
    condition:
        all of them
}
rule vm_object_transform {
    strings:
        $ = { 40 53 56 57 41 54 41 55 41 56 41 57 b8 ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 2b e0 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 }
    condition:
        all of them
}
rule trigger_rip {
    strings:
        $ = { 45 69 cc fb 38 67 07 41 8b c4 49 83 c7 80 24 10 41 c1 e9 19 f6 d8 1b c9 81 e1 77 3f 03 24 81 c1 }
    condition:
        all of them
}
rule cxx_throw {
    strings:
        $ = { 48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 20 48 8b fa 48 8b f1 48 8b c1 }
    condition:
        all of them
}
""")

pe = pefile.PE("../playplay/Spotify_1.2.88.485_g1012a6e0.dll", fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase

matches = rules.match("../playplay/Spotify_1.2.88.485_g1012a6e0.dll")
for match in matches:
    print(f"{match.rule}:")
    for string_match in match.strings:
        for instance in string_match.instances:
            va = image_base + instance.offset
            print(f"  Offset: {hex(instance.offset)}, VA: {hex(va)}")
