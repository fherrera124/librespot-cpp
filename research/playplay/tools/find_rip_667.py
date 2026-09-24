import yara

rules = yara.compile(source="""
rule key_extract {
    strings:
        // Match the core logic: add r15, -0x80; and al, 0x10; shr r9d, 0x19; neg al; sbb ecx, ecx
        $ = { 49 83 c7 ?? 24 10 41 c1 e9 19 f6 d8 1b c9 }
    condition:
        all of them
}
""")
for match in rules.match("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll"):
    for string_match in match.strings:
        for instance in string_match.instances:
            print(f"key_extract Offset: {hex(instance.offset)}")
