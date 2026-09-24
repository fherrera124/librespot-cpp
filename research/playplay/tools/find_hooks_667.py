import yara
import pefile

rules = yara.compile(source="""
rule mtx_lock { strings: $ = { 33 d2 e9 ?? ?? ?? ?? cc 48 83 ec 38 48 8d 54 24 } condition: all of them }
rule cnd_wait { strings: $ = { 40 53 48 83 ec 20 ff 4a 4c 48 8b da c7 42 48 ff } condition: all of them }
rule mtx_unlock { strings: $ = { 48 83 ec 28 83 69 4c 01 75 11 c7 41 48 ff ff ff } condition: all of them }
rule malloc { strings: $ = { e9 ?? ?? ?? ?? cc cc cc cc cc cc cc cc cc cc cc } condition: all of them }
rule fill_random { strings: $ = { e9 ?? ?? ?? ?? cc cc 48 89 5c 24 10 48 89 6c } condition: all of them }
rule cxx_throw { strings: $ = { 48 89 5c 24 18 48 89 74 24 20 57 48 83 ec 50 48 } condition: all of them }
""")

pe = pefile.PE("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll", fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase

matches = rules.match("../playplay/Spotify_1.2.93.667_g7b5cc0ce.dll")
for match in matches:
    print(f"{match.rule}:")
    for string_match in match.strings:
        for instance in string_match.instances:
            va = base + pe.get_rva_from_offset(instance.offset)
            print(f"  VA: {hex(va)}")
