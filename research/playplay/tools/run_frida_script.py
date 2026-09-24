import frida, sys, json, threading
done = threading.Event()
def on_message(message, data):
    if message["type"] == "send":
        print(json.dumps(message["payload"]))
        done.set()
    elif message["type"] == "error":
        print("ERROR:", message)
        done.set()

pid = int(sys.argv[1])
script_source = open(sys.argv[2]).read()

session = frida.attach(pid)
script = session.create_script(script_source)
script.on("message", on_message)
script.load()
done.wait()
session.detach()
