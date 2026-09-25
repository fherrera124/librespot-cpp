#!/usr/bin/env python3
"""
PlayPlay White-Box AES-128 DFA Extractor (Producción)
Este script orquesta el ataque completo:
1. Conecta con Frida al proceso de Spotify.
2. Inyecta la clave ofuscada y genera el contexto.
3. Inyecta fallos de un bit para recuperar cifrados erróneos.
4. Resuelve el DFA matemáticamente y recupera la Clave AES Maestra (K0).
"""

import argparse
import sys
import threading
import json
from pathlib import Path

# Añadimos el directorio tools al path para importar reverse_key_schedule
sys.path.insert(0, str(Path(__file__).parent))
from reverse_key_schedule import reverse_key_schedule

# --- TABLAS Y FUNCIONES MATEMÁTICAS DFA ---
SBOX = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
]
INV_SBOX = [0]*256
for i in range(256): INV_SBOX[SBOX[i]] = i

def galois_mult(a, b):
    p = 0
    for _ in range(8):
        if b & 1: p ^= a
        hi_bit_set = a & 0x80
        a <<= 1
        if hi_bit_set: a ^= 0x1b
        b >>= 1
    return p % 256

def get_column_indices(col_idx):
    if col_idx == 0: return [0, 13, 10, 7]
    if col_idx == 1: return [4, 1, 14, 11]
    if col_idx == 2: return [8, 5, 2, 15]
    if col_idx == 3: return [12, 9, 6, 3]

def get_mix_col_multipliers(row_idx):
    if row_idx == 0: return [2, 1, 1, 3]
    if row_idx == 1: return [3, 2, 1, 1]
    if row_idx == 2: return [1, 3, 2, 1]
    if row_idx == 3: return [1, 1, 3, 2]

# --- FRIDA PAYLOAD ---
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

def solve_dfa(correct_hex, faults):
    """Resuelve la Ronda 10 matemáticamente."""
    correct = bytes.fromhex(correct_hex)
    columns_faults = {0: [], 1: [], 2: [], 3: []}

    for f in faults:
        faulty = bytes.fromhex(f["ct"])
        diff = [i for i in range(16) if correct[i] != faulty[i]]
        if len(diff) == 4:
            for c in range(4):
                if sorted(diff) == sorted(get_column_indices(c)):
                    columns_faults[c].append(faulty)

    recovered_k10 = [None]*16

    for col in range(4):
        indices = get_column_indices(col)
        possible_keys = None
        
        for faulty in columns_faults[col]:
            current_possible = set()
            for fault_row in range(4):
                mults = get_mix_col_multipliers(fault_row)
                for e in range(1, 256):
                    diff_in = [galois_mult(e, m) for m in mults]
                    
                    valid_k = []
                    for i in range(4):
                        idx = indices[i]
                        c_corr = correct[idx]
                        c_flt = faulty[idx]
                        d_in = diff_in[i]
                        
                        valid_for_byte = []
                        for k in range(256):
                            if INV_SBOX[c_corr ^ k] ^ INV_SBOX[c_flt ^ k] == d_in:
                                valid_for_byte.append(k)
                        valid_k.append(valid_for_byte)
                    
                    if all(len(v) > 0 for v in valid_k):
                        for k0 in valid_k[0]:
                            for k1 in valid_k[1]:
                                for k2 in valid_k[2]:
                                    for k3 in valid_k[3]:
                                        current_possible.add((k0, k1, k2, k3))
            
            if possible_keys is None:
                possible_keys = current_possible
            else:
                possible_keys = possible_keys.intersection(current_possible)
                
        if possible_keys and len(possible_keys) == 1:
            k_tuple = list(possible_keys)[0]
            for i in range(4):
                recovered_k10[indices[i]] = k_tuple[i]

    if None in recovered_k10:
        return None
    return bytes(recovered_k10)

def main():
    import frida
    parser = argparse.ArgumentParser(description="Extractor de Clave AES-128 para PlayPlay usando DFA")
    parser.add_argument("--pid", type=int, required=True, help="PID del proceso Spotify.exe")
    parser.add_argument("--obfuscated", type=str, required=True, help="Clave ofuscada (hex) de 16 bytes")
    parser.add_argument("--b4", type=str, default="3a034bf8", help="Campo auxiliar b4_seq de 4 bytes")
    args = parser.parse_args()

    print("[*] PlayPlay White-Box AES-128 DFA Extractor")
    print(f"[-] Conectando al PID {args.pid}...")
    
    try:
        session = frida.attach(args.pid)
    except Exception as e:
        print(f"[!] Error al adjuntar Frida al proceso: {e}")
        return 1

    js_code = get_js_script(args.obfuscated, args.b4)
    done_event = threading.Event()
    extracted_data = {}

    def on_message(message, data):
        if message['type'] == 'error':
            print(f"[!] Error en Frida: {message.get('stack', str(message))}")
            done_event.set()
        elif message['type'] == 'send':
            payload = message['payload']
            if payload.get('type') == 'dfa_data':
                extracted_data['correct'] = payload['correct']
                extracted_data['faults'] = payload['faults']
                done_event.set()

    script = session.create_script(js_code)
    script.on('message', on_message)
    script.load()

    print("[-] Inyectando código de captura y generando fallos en memoria...")
    done_event.wait(timeout=10.0)

    try:
        script.unload()
        session.detach()
    except:
        pass

    if 'correct' not in extracted_data:
        print("[!] No se recibieron datos de DFA de Frida.")
        return 1

    print(f"[+] Bloque cifrado base capturado: {extracted_data['correct']}")
    print(f"[+] Se generaron {len(extracted_data['faults'])} trazas con fallos.")

    print("[-] Resolviendo ecuaciones DFA para recuperar Ronda 10...")
    k10_bytes = solve_dfa(extracted_data['correct'], extracted_data['faults'])
    
    if not k10_bytes:
        print("[!] Error: No se pudo resolver K10 completamente. Posible fallo en recolección de trazas.")
        return 1
        
    print(f"[+] Clave de Ronda 10 recuperada: {k10_bytes.hex()}")

    print("[-] Invirtiendo Key Schedule...")
    k0_bytes = reverse_key_schedule(list(k10_bytes))
    
    print("\n" + "="*50)
    print(f"[*] MASTER AES KEY EXTRAIDA: {k0_bytes.hex()}")
    print("="*50 + "\n")

    return 0

if __name__ == '__main__':
    sys.exit(main())
