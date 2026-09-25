"""Build gate for the two clean capturers; no Spotify RVA is executed here."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct

VERSION = "1.2.92.148"
DLL_SHA256 = "7b44456a90142daeb758e2736d8628ffe211d6ea523db8f1050b3c8b1addb68a"
# First 16 bytes from the hash-verified raw 148 dump. Compare before any hooks.
ENTRY_SIGNATURES = {
    "0x4a0268": "4055535657415441564157488dac24d0",
    "0x49eaa4": "4053565741544155415641574881ec20",
    "0x49f854": "40535556574154415541564157b84810",
    "0xd9e2e4": "48895c240848896c2410488974241857",
    "0xd9d0f0": "405355565741544155415641574883ec",
}

IDENTITY_SCRIPT = """
rpc.exports.describe = function () {
    const m = Process.getModuleByName('Spotify.dll');
    return {pid: Process.id, arch: Process.arch, platform: Process.platform,
            path: m.path, base: m.base.toString(), size: m.size};
};
"""


def file_version(path):
    """Read VS_FIXEDFILEINFO from the disk file using Windows version.dll."""
    if os.name != "nt":
        raise RuntimeError("Build preflight must run on Windows")
    from ctypes import wintypes

    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    api.GetFileVersionInfoW.restype = wintypes.BOOL
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    api.VerQueryValueW.restype = wintypes.BOOL
    unused = wintypes.DWORD()
    size = api.GetFileVersionInfoSizeW(str(path), ctypes.byref(unused))
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(size)
    if not api.GetFileVersionInfoW(str(path), 0, size, buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    value, length = ctypes.c_void_p(), wintypes.UINT()
    if not api.VerQueryValueW(buffer, "\\", ctypes.byref(value), ctypes.byref(length)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not value.value or length.value < 52:
        raise ValueError("Missing VS_FIXEDFILEINFO")
    fields = struct.unpack("<13I", ctypes.string_at(value, 52))
    if fields[0] != 0xFEEF04BD:
        raise ValueError("Invalid VS_FIXEDFILEINFO signature")
    ms, ls = fields[2:4]
    return f"{ms >> 16}.{ms & 65535}.{ls >> 16}.{ls & 65535}"


def verify_identity(identity, pid):
    if identity["pid"] != pid or identity["arch"] != "x64" or identity["platform"] != "windows":
        raise ValueError("Expected the selected Windows x64 process")
    path = Path(identity["path"])
    if path.name.lower() != "spotify.dll":
        raise ValueError("Unexpected loaded DLL path")
    before = path.stat()
    version = file_version(path)
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("DLL changed during preflight")
    if version != VERSION or digest != DLL_SHA256:
        raise ValueError(f"Unsupported Spotify.dll: version={version}, sha256={digest}")
    return dict(identity, version=version, sha256=digest)


def preflight(session, pid):
    probe = session.create_script(IDENTITY_SCRIPT)
    try:
        probe.load()
        return verify_identity(probe.exports_sync.describe(), pid)
    finally:
        probe.unload()


def guarded_source(source, identity):
    """Bind the verified disk build to the live module and check code bytes."""
    return "const playplay148Verified = " + json.dumps(identity) + ";\n" + """
function requireVerifiedBuild148() {
    const expected = playplay148Verified;
    const module = Process.getModuleByName('Spotify.dll');
    if (Process.id !== expected.pid || Process.arch !== 'x64' ||
        Process.platform !== 'windows' || module.path !== expected.path ||
        module.base.toString() !== expected.base || module.size !== expected.size) {
        throw new Error('Spotify module changed after preflight');
    }
    const signatures = """ + json.dumps(ENTRY_SIGNATURES) + """;
    for (const [rva, expectedBytes] of Object.entries(signatures)) {
        const actual = Array.from(new Uint8Array(module.base.add(parseInt(rva, 16))
            .readByteArray(16)), b => b.toString(16).padStart(2, '0')).join('');
        if (actual !== expectedBytes) throw new Error('RVA signature mismatch: ' + rva);
    }
    return module;
}
""" + source
