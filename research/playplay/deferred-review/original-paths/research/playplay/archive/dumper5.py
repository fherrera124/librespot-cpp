import ctypes
from ctypes import wintypes
import sys
import psutil

kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

def get_base():
    for p in psutil.process_iter(['pid', 'name']):
        if p.info['name'].lower() == 'spotify.exe':
            pid = p.info['pid']
            hProcess = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
            if hProcess:
                hMods = (wintypes.HMODULE * 1024)()
                cbNeeded = wintypes.DWORD()
                if psapi.EnumProcessModulesEx(hProcess, ctypes.byref(hMods), ctypes.sizeof(hMods), ctypes.byref(cbNeeded), 3):
                    for i in range(cbNeeded.value // ctypes.sizeof(wintypes.HMODULE)):
                        modName = ctypes.create_unicode_buffer(260)
                        psapi.GetModuleFileNameExW(hProcess, hMods[i], modName, ctypes.sizeof(modName))
                        if 'spotify.dll' in modName.value.lower():
                            kernel32.CloseHandle(hProcess)
                            return pid, hMods[i]
                kernel32.CloseHandle(hProcess)
    return None, None

pid, base_addr = get_base()
if not pid:
    print("Spotify.dll not found.")
    sys.exit(1)

print(f"Found in PID {pid} at {hex(base_addr)}")

with open("base_addr.txt", "w") as f:
    f.write(str(base_addr))

hProcess = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)

dos_header = ctypes.create_string_buffer(64)
bytesRead = ctypes.c_size_t(0)
kernel32.ReadProcessMemory(hProcess, base_addr, dos_header, 64, ctypes.byref(bytesRead))

e_lfanew = int.from_bytes(dos_header.raw[0x3C:0x40], 'little')
nt_headers = ctypes.create_string_buffer(256)
kernel32.ReadProcessMemory(hProcess, base_addr + e_lfanew, nt_headers, 256, ctypes.byref(bytesRead))

size_of_image = int.from_bytes(nt_headers.raw[0x50:0x54], 'little')
print(f"SizeOfImage: {hex(size_of_image)}")

buffer = ctypes.create_string_buffer(size_of_image)
res = kernel32.ReadProcessMemory(hProcess, base_addr, buffer, size_of_image, ctypes.byref(bytesRead))

with open("Spotify_dump_new.dll", "wb") as f:
    f.write(buffer.raw)
    
print("Dumped successfully.")
kernel32.CloseHandle(hProcess)

