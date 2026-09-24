def get_js_script(obfuscated_key: str, b4_seq: str) -> str:
    return f"""
    const mod = Process.getModuleByName('Spotify.dll');

    function hex(buffer) {{
        if (!buffer) return null;
        return Array.from(new Uint8Array(buffer), b => b.toString(16).padStart(2, '0')).join('');
    }}
    function allocateHex(value){{
        const p=Memory.alloc(value.length/2);
        p.writeByteArray(value.match(/../g).map(b=>parseInt(b,16)));
        return p;
    }}

    const pipelineAddress=mod.base.add(0x4a0268);
    const initAddress=mod.base.add(0xd9e2e4);
    const streamAddress=mod.base.add(0xd9d0f0);
    const options={{exceptions:'propagate',traps:'all'}};
    const pipeline=new NativeFunction(pipelineAddress,'void',['pointer','pointer','pointer','pointer'],options);
    const init=new NativeFunction(initAddress,'void',['pointer','pointer','pointer'],options);
    const stream=new NativeFunction(streamAddress,'void',['pointer','pointer'],options);

    setImmediate(() => {{
        try {{
            const obfuscated = '{obfuscated_key}';
            const b4_seq = '{b4_seq}';
            
            const request=Memory.alloc(256),input=allocateHex(obfuscated),auxiliary=Memory.alloc(4);
            request.writeByteArray(new Uint8Array(256));
            request.add(0x70).writeU8(1); 
            auxiliary.writeByteArray(b4_seq.match(/../g).map(b=>parseInt(b,16)));
            
            let generated28 = null;
            let ours = false;
            const hook = Interceptor.attach(mod.base.add(0x49eaa4), {{
                onEnter(args) {{ this.ours = true; this.output = args[2]; }},
                onLeave() {{ if (this.ours) generated28 = this.output.readByteArray(28); }}
            }});

            pipeline(request, input, auxiliary, ptr(0));
            hook.detach();

            if (!generated28) {{
                send({{type: 'error', stack: 'Fallo al generar descriptor 28 bytes'}});
                return;
            }}

            const context = Memory.alloc(4096);
            const wrapped = Memory.alloc(28);
            wrapped.writeByteArray(generated28);
            init(context, wrapped, auxiliary);

            const output = Memory.alloc(32);
            const cleanContext = Memory.alloc(740);
            Memory.copy(cleanContext, context, 740);

            stream(context, output);
            const correct_ct = hex(output.readByteArray(16));

            let faulty_cts = [];

            for (let offset = 0x40; offset < 0xDF; offset++) {{
                Memory.copy(context, cleanContext, 740);
                const val = context.add(offset).readU8();
                context.add(offset).writeU8(val ^ 1);
                stream(context, output);
                const fault_ct = hex(output.readByteArray(16));
                if (fault_ct !== correct_ct) {{
                    faulty_cts.push({{
                        offset: offset.toString(16),
                        ct: fault_ct
                    }});
                }}
            }}

            send({{
                type: 'dfa_data',
                correct: correct_ct,
                faults: faulty_cts
            }});

        }} catch (e) {{
            send({{type: 'error', stack: e.toString()}});
        }}
    }});
    """

lines = get_js_script('A', 'B').split('\n')
for i, line in enumerate(lines):
    print(f"{i+1}: {line}")
