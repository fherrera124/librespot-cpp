import yara
import pefile

rules = yara.compile(source="""
rule cxx_throw {
    strings:
        $ = { 48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 20 48 8b fa 48 8b f1 48 8b c1 }
    condition:
        all of them
}
""")
pe = pefile.PE("../playplay/Spotify_1.2.88.485_g1012a6e0.dll", fast_load=True)
for match in rules.match("../playplay/Spotify_1.2.88.485_g1012a6e0.dll"):
    for string_match in match.strings:
        for instance in string_match.instances:
            print(f"cxx_throw VA: {hex(pe.OPTIONAL_HEADER.ImageBase + instance.offset)}")
