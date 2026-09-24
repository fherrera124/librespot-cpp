from pathlib import Path
import frida, psutil, time, sys
def on_message(message, data):
    if message['type'] == 'send': print(message['payload'])
    else: print(message)

with Path(__file__).with_name('hook_wsasend.js').open(encoding='utf-8') as f: js = f.read()
sessions = []
for p in psutil.process_iter(['pid', 'name']):
    if p.info['name'] and p.info['name'].lower() == 'spotify.exe':
        try:
            s = frida.attach(p.info['pid'])
            script = s.create_script(js)
            script.on('message', on_message)
            script.load()
            sessions.append(s)
        except Exception as e:
            print(e)
print(f"Attached to {len(sessions)} PIDs. Waiting 30 seconds...")
time.sleep(30)
for s in sessions:
    try: s.detach()
    except: pass
