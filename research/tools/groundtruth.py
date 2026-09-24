"""Pide keys para los 5 file_id cuya AES key ya conocemos (vectores publicados),
usando NUESTRO token E. Da verdad conocida sin CDN ni descifrado."""
import json, os, time, urllib.request, urllib.error
B=os.environ["SP_BEARER"].removeprefix("Bearer ").strip(); C=os.environ["SP_CLIENT_TOKEN"].strip()
E="01f62e56cd5435b90dde1a4fdf42af2d"
KNOWN=[("2f43127d80edc9cd9f12f441e1cb7904b680f9da","a503a84c1dc9271460cc13f142e0bae2"),
       ("1a8e5b04837957617162724232b0c96922222447","c3206271b4c70fff8e4ac3993c4dae8a"),
       ("cf1bd197a6f5d613fc856bd689e43c0f4069b800","8d86fb522c00729f35b34d60b165b922"),
       ("71df45edb4748a8b1bd4126ded06063674745182","0fd3998b706247b3474b2d3cf6d8e31f"),
       ("894813d0a3113c97ab601b0f65afb9543d96ec32","4d442c155f9a95258f613a89be957be9")]
def vi(n):
    o=b""
    while True:
        x,n=n&0x7f,n>>7; o+=bytes([x|(0x80 if n else 0)])
        if not n: return o
def body(v):
    t=bytes.fromhex(E)
    return b"\x08"+vi(v)+b"\x12"+vi(len(t))+t+b"\x20"+vi(1)+b"\x28"+vi(1)
def post(fid,d):
    r=urllib.request.Request(f"https://gew4-spclient.spotify.com/playplay/v1/key/{fid}",data=d,method="POST")
    r.add_header("Authorization","Bearer "+B); r.add_header("Client-Token",C); r.add_header("Content-Type","application/x-protobuf")
    try:
        with urllib.request.urlopen(r,timeout=25) as x: return x.status,x.read()
    except urllib.error.HTTPError as e: return e.code,e.read()
    except Exception as e: return -1,str(e).encode()[:40]
out={}
for fid,aes in KNOWN:
    row=[]
    for v in (2,3,4,5):
        s,b=post(fid,body(v))
        if s==200 and b[:1]==b"\x0a":
            k=b[2:2+b[1]].hex(); row.append(f"v{v}={k}"); out.setdefault(fid,{})[f"v{v}"]=k
        else: row.append(f"v{v}:{s}")
        time.sleep(0.2)
    out.setdefault(fid,{})["aes"]=aes
    print(f"{fid[:10]}… aes={aes}")
    for r in row: print("   ",r)
json.dump(out,open("groundtruth_tokenE.json","w"),indent=1)
print("\nguardado groundtruth_tokenE.json")
