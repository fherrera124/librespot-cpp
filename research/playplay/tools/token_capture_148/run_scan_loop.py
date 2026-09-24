from pathlib import Path
import frida, psutil, time, sys

def hexdump(src, length=16):
    result = []
    for i in range(0, len(src), length):
        s = src[i:i+length]
        hexa = b' '.join([b"%02X" % x for x in s])
        text = b''.join([bytes([x]) if 0x20 <= x < 0x7F else b'.' for x in s])
        result.append(b"%04X   %-*s   %s" % (i, length*(3), hexa, text))
    return b'\n'.join(result).decode('utf-8', errors='ignore')

def on_message(message, data):
    if message['type'] == 'send':
        payload = message['payload']
        if payload.get('type') == 'dump':
            print(f"\n--- Found at {payload['address']} (PID {payload['pid']}) ---")
            print(hexdump(bytes.fromhex(payload['data'])))
        else:
            print(payload)
    else: print(message)

with Path(__file__).with_name('scan_playplay_loop.js').open(encoding='utf-8') as f: js = f.read()
sessions = []
for p in psutil.process_iter(['pid', 'name']):
    if p.info['name'] and p.info['name'].lower() == 'spotify.exe':
        try:
            s = frida.attach(p.info['pid'])
            script = s.create_script(js)
            script.on('message', on_message)
            script.load()
            sessions.append(s)
        except Exception as e: pass
print(f"Attached to {len(sessions)} PIDs. Waiting 30 seconds...")
time.sleep(30)
for s in sessions:
    try: s.detach()
    except: pass
