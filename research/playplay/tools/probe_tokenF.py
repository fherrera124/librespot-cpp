import os, time, urllib.request, urllib.error
B=os.environ["SP_BEARER"].removeprefix("Bearer ").strip(); C=os.environ["SP_CLIENT_TOKEN"].strip()
FID="f5eb2b3e7a3798b4a55369bd3cc840fceddebb94"
def vi(n):
    o=b""
    while True:
        x,n=n&0x7f,n>>7; o+=bytes([x|(0x80 if n else 0)])
        if not n: return o
def body(v,th):
    t=bytes.fromhex(th)
    return b"\x08"+vi(v)+b"\x12"+vi(len(t))+t+b"\x20"+vi(1)+b"\x28"+vi(1)
def post(d):
    r=urllib.request.Request(f"https://gew4-spclient.spotify.com/playplay/v1/key/{FID}",data=d,method="POST")
    r.add_header("Authorization","Bearer "+B); r.add_header("Client-Token",C); r.add_header("Content-Type","application/x-protobuf")
    try:
        with urllib.request.urlopen(r,timeout=25) as x: return x.status,x.read()
    except urllib.error.HTTPError as e: return e.code,e.read()
    except Exception as e: return -1,str(e).encode()[:40]
TOKENS=[("F another/483","027b23a2442c86ca4b004ddfef291954"),
        ("E wavee /483 ","01f62e56cd5435b90dde1a4fdf42af2d")]
print("token".ljust(15)+"".join(f" v{v}".ljust(9) for v in (2,3,4,5)))
res={}
for lbl,th in TOKENS:
    cells=[]
    for v in (2,3,4,5):
        s,b=post(body(v,th))
        if s==200 and len(b)>=2 and b[0]==0x12:
            key=b[2:2+b[1]].hex(); cells.append("200"); res[(lbl.strip(),v)]=key
        else: cells.append(str(s))
        time.sleep(0.2)
    print(lbl.ljust(15)+"".join(c.ljust(9) for c in cells))
print("\nkeys ofuscadas (mismo file, comparables):")
for (lbl,v),k in sorted(res.items()):
    print(f"  {lbl:14} v{v}  {k}")
