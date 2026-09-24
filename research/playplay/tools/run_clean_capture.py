#!/usr/bin/env python3
"""Run clean capture without injecting vectors (Windows build 148)."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import threading
from playplay_148_preflight import guarded_source, preflight

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="Verified main Spotify process")
    parser.add_argument("--script", type=Path, required=True, help="Clean capture JS")
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    import frida
    import psutil

    process = psutil.Process(args.pid)
    if process.name().lower() != "spotify.exe" or any(
        '--type=' in part.lower() or 'crashpad' in part.lower() for part in process.cmdline()
    ):
        parser.error("PID must belong to the main Spotify.exe process")

    source = args.script.read_text(encoding='utf-8')
    
    report = {"started_at": datetime.now(timezone.utc).isoformat(), "pid": args.pid,
              "mode": "clean_capture",
              "script_file": args.script.name,
              "script_sha256": hashlib.sha256(source.encode('utf-8')).hexdigest(),
              "events": []}
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
        report['events'].append(payload)
        print(json.dumps(payload), flush=True)
        if payload.get('type') == 'error':
            report['error'] = payload.get('stack', 'Capture failed')
            done.set()
        elif payload.get('type') == 'clean_capture_done':
            done.set()

    def on_detached(reason, crash):
        if not done.is_set():
            report['error'] = f'Session detached: {reason}'
            done.set()

    with args.report.open('x', encoding='utf-8') as output:
        try:
            session = frida.attach(args.pid)
            session.on('detached', on_detached)
            report['preflight'] = preflight(session, args.pid)
            executed_source = guarded_source(source, report['preflight'])
            report['executed_source_sha256'] = hashlib.sha256(executed_source.encode('utf-8')).hexdigest()
            script = session.create_script(executed_source)
            script.on('message', on_message)
            script.load()
            print('Build verified; running the scripted capture.', flush=True)
            print('Press Ctrl+C to stop capture and save report.', flush=True)
            if not done.wait(args.timeout):
                report['error'] = 'Capture timed out before completion'
        except KeyboardInterrupt:
            report['error'] = 'Capture interrupted'
            print("Capture stopped by user.", flush=True)
        except Exception as exc:
            report['error'] = str(exc) or type(exc).__name__
        finally:
            done.set()
            if script:
                try:
                    script.unload()
                except Exception:
                    report.setdefault('error', 'Failed to unload capture script')
            if session:
                try:
                    session.detach()
                except Exception:
                    report.setdefault('error', 'Failed to detach Frida session')
            if not any(e.get('type') == 'clean_capture_done' for e in report['events']):
                report.setdefault('error', 'Capture did not complete')
            report['finished_at'] = datetime.now(timezone.utc).isoformat()
            json.dump(report, output, indent=2)
            output.write('\n')
    
    return 1 if report.get('error') else 0

if __name__ == '__main__':
    sys.exit(main())
