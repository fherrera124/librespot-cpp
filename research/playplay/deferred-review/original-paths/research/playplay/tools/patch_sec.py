import sys
import pefile

dll_path = sys.argv[1]
pe = pefile.PE(dll_path)

# Clear Security Directory
pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_SECURITY']].VirtualAddress = 0
pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_SECURITY']].Size = 0

pe.write(dll_path + ".patched.dll")
print(f"Patched {dll_path}.patched.dll")
