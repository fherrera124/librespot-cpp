#!/usr/bin/env python3
"""Run saved vectors inside the first VmObjectTransform call (Windows build 148)."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import threading


def make_script(vectors, exceptions="steal"):
    return "var exceptionMode = " + json.dumps(exceptions) + ";\nvar vectors = " + json.dumps(vectors) + ";\n" + r"""
const mod = Process.findModuleByName('Spotify.dll');
if (mod === null) throw new Error('Spotify.dll not loaded');
const address = mod.base.add(0x49EAA4);
const transform = new NativeFunction(address, 'void', ['pointer', 'pointer', 'pointer', 'pointer'], {exceptions: exceptionMode});
let started = false;
function fromHex(hex) {
    return hex.match(/../g).map(pair => parseInt(pair, 16));
}
function toHex(buffer) {
    return Array.from(new Uint8Array(buffer), byte => byte.toString(16).padStart(2, '0')).join('');
}
function readOutput(output) {
    const raw = toHex(output.readByteArray(24));
    const begin = output.readPointer();
    const end = output.add(Process.pointerSize).readPointer();
    if (!begin.isNull() && end.sub(begin).equals(ptr(16))) {
        return {raw: raw, format: 'vector16', actual: toHex(begin.readByteArray(16))};
    }
    return {raw: raw, format: 'inline16_candidate', actual: raw.slice(0, 32)};
}
Interceptor.attach(address, {
    onEnter(args) {
        if (started) return;
        started = true;
        this.originalCall = true;
        this.output = args[2];
        try {
            const snapshot = args[0].readByteArray(144);
            const originalArg3 = args[3];
            const naturalInput = args[1].readByteArray(16);
            send({type: 'captured', module: mod.path, base: mod.base.toString(), rva: '0x49EAA4',
                  input: toHex(naturalInput), init: toHex(originalArg3.readByteArray(16))});
            const controlVm = Memory.alloc(144);
            controlVm.writeByteArray(snapshot);
            const controlInput = Memory.alloc(16);
            controlInput.writeByteArray(naturalInput);
            const controlOutput = Memory.alloc(32);
            try {
                transform(controlVm, controlInput, controlOutput, originalArg3);
                send({type: 'control_replay', ...readOutput(controlOutput)});
            } catch (e) {
                send({type: 'fatal', error: 'Natural input replay failed: ' + e.toString()});
                return;
            }
            for (let i = 0; i < vectors.length; i++) {
                const vm = Memory.alloc(144);
                vm.writeByteArray(snapshot);
                const input = Memory.alloc(16);
                input.writeByteArray(fromHex(vectors[i].obfuscated));
                const output = Memory.alloc(32);
                try {
                    transform(vm, input, output, originalArg3);
                    send({type: 'result', index: i, ...readOutput(output)});
                } catch (e) {
                    send({type: 'vector_error', index: i, error: e.toString()});
                    break;
                }
            }
        } catch (e) {
            send({type: 'fatal', error: e.toString()});
        }
    },
    onLeave(retval) {
        if (!this.originalCall) return;
        try {
            send({type: 'natural_output', ...readOutput(this.output)});
        } catch (e) {
            send({type: 'fatal', error: 'Natural output read failed: ' + e.toString()});
        }
        send({type: 'done'});
    }
});
send({type: 'attached', module: mod.path, base: mod.base.toString(), rva: '0x49EAA4'});
"""


def load_vectors(path):
    raw = path.read_bytes()
    vectors = []
    for item in json.loads(raw)["vectors_token_E"]:
        for version, obfuscated in item["obfuscated"].items():
            for value in (obfuscated, item["aes"]):
                if len(value) != 32 or len(bytes.fromhex(value)) != 16:
                    raise ValueError("Invalid vector key")
            vectors.append({"file_id": item["file_id"], "version": version,
                            "obfuscated": obfuscated, "expected": item["aes"]})
    if not vectors:
        raise ValueError("No vectors found")
    return vectors, hashlib.sha256(raw).hexdigest()


def assess_report(report, planned):
    counts = {status: sum(item['status'] == status for item in report['results'])
              for status in ('pass', 'mismatch', 'error')}
    replay = next((event for event in report['events'] if event['type'] == 'control_replay'), None)
    natural = next((event for event in report['events'] if event['type'] == 'natural_output'), None)
    control_pass = bool(replay and natural and replay['actual'] == natural['actual'])
    report['summary'] = {'planned': planned, 'completed': len(report['results']),
                         **counts, 'control_pass': control_pass}
    if report.get('error') or counts['error'] or len(report['results']) != planned:
        report['assessment'] = 'inconclusive_execution_error'
        return 2
    if not control_pass:
        report['assessment'] = 'inconclusive_control_failed'
        return 2
    report['assessment'] = 'candidate_mismatch' if counts['mismatch'] else 'all_vectors_match'
    return 1 if counts['mismatch'] else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="Verified main Spotify process")
    parser.add_argument("--vectors", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--exceptions", choices=("steal", "propagate"), default="steal")
    parser.add_argument("--script", type=Path, help="Alternative diagnostic JS, with vectors injected")
    parser.add_argument("--script-data", type=Path, help="JSON injected as diagnosticData for an alternative script")
    parser.add_argument("--advance-track", action="store_true", help="Post Next Track to this Spotify process window")
    args = parser.parse_args()
    if args.script_data and not args.script:
        parser.error("--script-data requires --script")
    if not 0 < args.timeout < float('inf'):
        parser.error("--timeout must be positive and finite")
    vectors, digest = load_vectors(args.vectors)
    import frida
    import psutil

    process = psutil.Process(args.pid)
    if process.name().lower() != "spotify.exe" or any(
        '--type=' in part.lower() or 'crashpad' in part.lower() for part in process.cmdline()
    ):
        parser.error("PID must belong to the main Spotify.exe process")

    source = ("var vectors = " + json.dumps(vectors) + ";\n" + args.script.read_text()
              if args.script else make_script(vectors, args.exceptions))
    if args.script_data:
        source = "var diagnosticData = " + json.dumps(json.loads(args.script_data.read_text())) + ";\n" + source
    if args.advance_track:
        source += '\n' + Path(__file__).with_name('advance_spotify_track.js').read_text()
    report = {"started_at": datetime.now(timezone.utc).isoformat(), "pid": args.pid,
              "vectors_sha256": digest,
              "mode": "alternative_script" if args.script else "onEnter",
              "script_file": args.script.name if args.script else None,
              "exceptions": args.exceptions,
              "script_sha256": hashlib.sha256(source.encode()).hexdigest(),
              "results": [], "events": []}
    done = threading.Event()
    session = None
    script = None

    def on_message(message, data):
        if message['type'] == 'error':
            report['error'] = message.get('stack', str(message))
            done.set()
            return
        if message['type'] != 'send':
            return
        payload = message['payload']
        kind = payload.get('type')
        if kind in ('result', 'vector_error'):
            item = dict(vectors[payload['index']])
            if kind == 'vector_error':
                item.update(status='error', error=payload['error'])
            else:
                item.update(actual=payload['actual'], status=(
                    'pass' if payload['actual'] == item['expected'] else 'mismatch'))
                item.update(raw=payload['raw'], format=payload['format'])
                for key in ('repeat_actual', 'repeat_raw'):
                    if key in payload:
                        item[key] = payload[key]
            report['results'].append(item)
            print(json.dumps(item), flush=True)
        else:
            report['events'].append(payload)
            if kind == 'copies':
                print(json.dumps({'type': kind, 'label': payload['label'],
                                  'count': len(payload['copies']), 'dropped': payload['dropped'],
                                  'match_count': len(payload.get('matches', []))}), flush=True)
            else:
                print(json.dumps(payload), flush=True)
            if kind == 'fatal':
                report['error'] = payload['error']
            elif kind == 'done':
                done.set()

    def on_detached(reason, crash):
        if not done.is_set():
            report['error'] = f'Session detached: {reason}'
            done.set()

    with args.report.open('x', encoding='utf-8') as output:
        try:
            session = frida.attach(args.pid)
            session.on('detached', on_detached)
            script = session.create_script(source)
            script.on('message', on_message)
            script.load()
            print('Play a different song in Windows Spotify to trigger validation.', flush=True)
            if not done.wait(args.timeout):
                report['error'] = 'Timed out waiting for the playback hook'
        except (Exception, KeyboardInterrupt) as exc:
            report['error'] = str(exc) or type(exc).__name__
        finally:
            done.set()
            if script:
                try:
                    script.unload()
                except Exception:
                    pass
            if session:
                try:
                    session.detach()
                except Exception:
                    pass
            exit_code = assess_report(report, len(vectors))
            report['finished_at'] = datetime.now(timezone.utc).isoformat()
            json.dump(report, output, indent=2)
            output.write('\n')
    print(json.dumps(report['summary']), flush=True)
    return exit_code


if __name__ == '__main__':
    sys.exit(main())
