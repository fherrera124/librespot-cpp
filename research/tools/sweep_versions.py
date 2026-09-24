"""Barrido token x version. Cierra el hueco de sweep_final.py, que hardcodeaba version=2.
Credenciales por variable de entorno: nunca se escriben ni se imprimen.
uso: SP_BEARER=... SP_CLIENT_TOKEN=... python3 sweep_versions.py"""
import os, sys, time, urllib.request, urllib.error

BEARER = os.environ.get("SP_BEARER", "").removeprefix("Bearer ").strip()
CLIENT = os.environ.get("SP_CLIENT_TOKEN", "").strip()
if not BEARER or not CLIENT:
    sys.exit("faltan SP_BEARER y/o SP_CLIENT_TOKEN en el entorno")

TOKENS = [
    ("C unplayplay/485", "02811027c51620c0fd36cd1de59e227a"),
    ("E wavee/483 (ctl)", "01f62e56cd5435b90dde1a4fdf42af2d"),
    ("A re-unplayplay  ", "011bf34c8d0393bcda8b1d3eeaf8f3b2"),
    ("B nuestro viejo  ", "0132b6f3165865ff69a47d4321ff7520"),
    ("D uhwot          ", "01e132cae527bd21620e822f58514932"),
]
VERSIONS = [1, 2, 3, 4, 5, 6, 7, 8]
FID = "f5eb2b3e7a3798b4a55369bd3cc840fceddebb94"   # el mismo de siempre, para comparar

def varint(n):
    o = b""
    while True:
        x, n = n & 0x7F, n >> 7
        o += bytes([x | (0x80 if n else 0)])
        if not n:
            return o

def body(ver, token_hex):
    t = bytes.fromhex(token_hex)
    return (b"\x08" + varint(ver) + b"\x12" + varint(len(t)) + t +
            b"\x20" + varint(1) + b"\x28" + varint(1))

def post(data):
    r = urllib.request.Request(
        f"https://gew4-spclient.spotify.com/playplay/v1/key/{FID}", data=data, method="POST")
    r.add_header("Authorization", "Bearer " + BEARER)
    r.add_header("Client-Token", CLIENT)
    r.add_header("Content-Type", "application/x-protobuf")
    try:
        with urllib.request.urlopen(r, timeout=25) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return -1, str(e).encode()[:40]

print(f"file_id {FID}\n")
print("token".ljust(19) + "".join(f"  v{v}".ljust(8) for v in VERSIONS))
hits = []
for label, th in TOKENS:
    cells = []
    for v in VERSIONS:
        st, b = post(body(v, th))
        if st == 200 and len(b) >= 2 and b[0] == 0x12:
            key = b[2:2 + b[1]].hex()
            cells.append("200*".ljust(8))
            hits.append((label, v, key))
        else:
            cells.append(str(st).ljust(8))
        time.sleep(0.25)
    print(label.ljust(19) + "".join("  " + c for c in cells), flush=True)

print()
if hits:
    print("keys ofuscadas obtenidas:")
    for label, v, key in hits:
        print(f"  {label.strip():18} v{v}  {key}")
else:
    print("ninguna combinacion devolvio 200")
