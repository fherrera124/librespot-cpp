import frida
import sys
import json
import time
import threading
import queue

js_code = """
var base = Process.findModuleByName("Spotify.dll").base;
var VmObjectTransformAddress = base.add(0x49EAA4);
var VmObjectTransform = new NativeFunction(VmObjectTransformAddress, 'void', ['pointer', 'pointer', 'pointer', 'pointer']);

function hexToBytes(hex) {
    var bytes = [];
    for (var c = 0; c < hex.length; c += 2)
        bytes.push(parseInt(hex.substr(c, 2), 16));
    return bytes;
}

function bytesToHex(buffer) {
    return Array.prototype.map.call(new Uint8Array(buffer), x => ('00' + x.toString(16)).slice(-2)).join('');
}

Interceptor.attach(VmObjectTransformAddress, {
    onEnter: function(args) {
        var vm_obj = args[0];
        var arg3 = args[3];
        
        send({type: "ready"});
        
        while (true) {
            var op = recv('decode', function(msg) {
                if (msg.hexToken === "exit") {
                    return;
                }
                var inBuffer = Memory.alloc(16);
                var outBuffer = Memory.alloc(32);
                inBuffer.writeByteArray(hexToBytes(msg.hexToken));
                
                try {
                    VmObjectTransform(vm_obj, inBuffer, outBuffer, arg3);
                    var aesKeyBuffer = outBuffer.readByteArray(16);
                    send({type: "result", aes_key: bytesToHex(aesKeyBuffer)});
                } catch(e) {
                    send({type: "result", aes_key: "ERROR_" + e.toString()});
                }
            });
            op.wait();
        }
    }
});
"""

def main():
    print("Loading vectors...")
    with open("ground-truth-vectors.json", "r") as f:
        data = json.load(f)

    result_queue = queue.Queue()
    ready_event = threading.Event()
    active_script = [None]

    def make_on_message(script):
        def on_message(message, data_payload):
            if message['type'] == 'send':
                payload = message['payload']
                if payload['type'] == 'ready':
                    active_script[0] = script
                    ready_event.set()
                elif payload['type'] == 'result':
                    result_queue.put(payload['aes_key'])
            elif message['type'] == 'error':
                print("JS Error:", message)
        return on_message

    processes = [p for p in frida.get_local_device().enumerate_processes() if "spotify" in p.name.lower()]
    for p in processes:
        try:
            s = frida.attach(p.pid)
            scr = s.create_script(js_code)
            scr.on('message', make_on_message(scr))
            scr.load()
            print(f"Attached to {p.pid}")
        except Exception as e:
            pass

    print("Please play a song in Spotify to trigger the hook...", flush=True)
    ready_event.wait()
    print("Audio thread intercepted! Running vectors...", flush=True)
    
    passed = 0
    total = 0
    script = active_script[0]
    
    for item in data["vectors_token_E"]:
        expected_aes = item["aes"]
        for version, obfuscated_key in item["obfuscated"].items():
            total += 1
            script.post({'type': 'decode', 'hexToken': obfuscated_key})
            try:
                res = result_queue.get(timeout=5)
                if res == expected_aes:
                    print(f"PASS: {obfuscated_key} -> {res}", flush=True)
                    passed += 1
                else:
                    print(f"FAIL: {obfuscated_key} -> {res} (Expected {expected_aes})", flush=True)
            except Exception as e:
                print(f"FAIL: timeout or error on {obfuscated_key}", flush=True)

    print(f"Result: {passed}/{total} passed.", flush=True)
    script.post({'type': 'decode', 'hexToken': 'exit'})
    sys.exit(0)

if __name__ == '__main__':
    main()
