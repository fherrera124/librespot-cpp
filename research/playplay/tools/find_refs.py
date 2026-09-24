import pefile

pe = pefile.PE("../playplay/Spotify_1.2.88.485_g1012a6e0.dll", fast_load=True)
data = pe.get_memory_mapped_image()

# target RVA in 485 is 0x178a040
target = 0x178a040

for i in range(0, len(data) - 4):
    # search for rip-relative references (e.g. lea reg, [rip+offset] or mov reg, [rip+offset])
    # The offset is a signed 32-bit int.
    # Instruction is usually 7 bytes. offset is at bytes 3-6.
    pass
