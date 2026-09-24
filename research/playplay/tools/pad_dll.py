import sys
import pefile

dll_path = sys.argv[1]
pe = pefile.PE(dll_path, fast_load=True)

size_of_image = pe.OPTIONAL_HEADER.SizeOfImage

with open(dll_path, "rb") as f:
    data = bytearray(f.read())

if len(data) < size_of_image:
    data.extend(b"\x00" * (size_of_image - len(data)))

with open(dll_path + ".padded.dll", "wb") as f:
    f.write(data)

print(f"Padded to {size_of_image} bytes.")
