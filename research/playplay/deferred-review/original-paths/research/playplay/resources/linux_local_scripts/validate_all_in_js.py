import frida
import sys
import json
import time

with open("ground-truth-vectors.json", "r") as f:
    data = json.load(f)

vectors = []
expected_aes_list = []
for item in data["vectors_token_E"]:
    for version, obfuscated_key in item["obfuscated"].items():
        vectors.append(obfuscated_key)
        expected_aes_list.append(item["aes"])

js_code = f"""
var base = Process.findModuleByName("Spotify.dll").base;
var VmObjectTransformAddress = base.add(0x49EAA4);
var VmObjectTransform = new NativeFunction(VmObjectTransformAddress, 'void', ['pointer', 'pointer', 'pointer', 'pointer']);

function hexToBytes(hex) {{
    var bytes = [];
    for (var c = 0; c < hex.length; c += 2)
        bytes.push(parseInt(hex.substr(c, 2), 16));
    return bytes;
}}

function bytesToHex(buffer) {{
    return Array.prototype.map.call(new Uint8Array(buffer), x => ('00' + x.toString(16)).slice(-2)).join('');
}}

var vectors = {json.dumps(vectors)};
var expected_aes = {json.dumps(expected_aes_list)};
var completed = false;

Interceptor.attach(VmObjectTransformAddress, {{
    onEnter: function(args) {{
        if (completed) return;
        completed = true;
        
        var orig_vm_obj = args[0];
        var orig_vm_obj_bytes = orig_vm_obj.readByteArray(144);
        var arg3 = args[3];
        
        var passed = 0;
        
        for (var i = 0; i < vectors.length; i++) {{
            var inBuffer = Memory.alloc(16);
            var outBuffer = Memory.alloc(32);
            inBuffer.writeByteArray(hexToBytes(vectors[i]));
            
            // Restore vm_obj state
            var temp_vm_obj = Memory.alloc(144);
            temp_vm_obj.writeByteArray(orig_vm_obj_bytes);
            
            try {{
                VmObjectTransform(temp_vm_obj, inBuffer, outBuffer, arg3);
                var aes = bytesToHex(outBuffer.readByteArray(16));
                send({{type: "result", index: i, aes: aes, expected: expected_aes[i], obfuscated: vectors[i]}});
            }} catch(e) {{
                send({{type: "error", index: i, msg: e.toString(), obfuscated: vectors[i]}});
            }}
        }}
        send({{type: "done"}});
    }}
}});
"""

def main():
    def on_message(message, data_payload):
        if message['type'] == 'send':
            payload = message['payload']
            if payload['type'] == 'result':
                if payload['aes'] == payload['expected']:
                    print(f"PASS: {payload['obfuscated']} -> {payload['aes']}")
                else:
                    print(f"FAIL: {payload['obfuscated']} -> {payload['aes']} (Expected {payload['expected']})")
            elif payload['type'] == 'done':
                sys.exit(0)
        elif message['type'] == 'error':
            print("Error:", message)

    processes = [p for p in frida.get_local_device().enumerate_processes() if "spotify" in p.name.lower()]
    for p in processes:
        try:
            s = frida.attach(p.pid)
            scr = s.create_script(js_code)
            scr.on('message', on_message)
            scr.load()
        except Exception as e:
            pass

    print("Please play a song in Spotify to trigger the hook...")
    time.sleep(86400)

if __name__ == '__main__':
    main()
