"""Read-only verification of this deployment; writes a new local evidence report."""
from datetime import datetime, timezone
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from urllib import request

run = Path(__file__).resolve().parents[1]
tools = Path("research/playplay/tools")
names = [
    "playplay_dfa_service.py", "playplay_dfa_rpc.js", "playplay_148_preflight.py",
    "extractor.py", "reverse_key_schedule.py", "start_playplay_service.ps1",
    "stop_playplay_service.ps1", "install_playplay_service.ps1",
    "playplay_service_requirements.txt", "test_playplay_dfa_service.py",
]
remote_dir = "C:/Users/francisco.herrera/playplay-service-20260925"
paths = ",".join("'" + remote_dir + "/" + name + "'" for name in names)
command = (
    "$ErrorActionPreference='Stop'; $files = @(" + paths + "); "
    "$hashes = @(Get-FileHash -LiteralPath $files -Algorithm SHA256 | Select-Object Path,Hash); "
    "$task = Get-ScheduledTask -TaskName CspotPlayPlayDfa; "
    "$listener = @(Get-NetTCPConnection -State Listen -LocalPort 8765 | "
    "Select-Object LocalAddress,LocalPort,OwningProcess); "
    "$processes = @(Get-CimInstance Win32_Process | Where-Object { "
    "$_.Name -eq 'python.exe' -and $_.CommandLine -notmatch 'lsp_server' } | "
    "Select-Object ProcessId,ParentProcessId,CommandLine); "
    "@{hashes=$hashes;task_state=$task.State.ToString(); "
    "action=$task.Actions;listeners=$listener;processes=$processes} | ConvertTo-Json -Depth 5"
)
completed = subprocess.run([
    "ssh", "-i", str(Path.home() / ".ssh/win_claude"), "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=8", "francisco.herrera@10.16.150.154",
    'powershell -NoProfile -Command "' + command + '"',
], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
if completed.returncode:
    raise RuntimeError(completed.stderr)
remote = json.loads(completed.stdout)
remote_hashes = {row["Path"].replace("\\", "/").rsplit("/", 1)[-1]: row["Hash"].lower()
                 for row in remote["hashes"]}
files = []
for name in names:
    local_hash = hashlib.sha256((tools / name).read_bytes()).hexdigest()
    files.append({"file": name, "local_sha256": local_hash,
                  "remote_sha256": remote_hashes.get(name),
                  "match": remote_hashes.get(name) == local_hash})
opener = request.build_opener(request.ProxyHandler({}))
with opener.open("http://127.0.0.1:8765/health", timeout=5) as response:
    health = json.load(response)
report = {"recorded_at": datetime.now(timezone.utc).isoformat(), "files": files,
          "all_match": all(row["match"] for row in files), "health": health,
          "windows": remote}
parser = argparse.ArgumentParser()
parser.add_argument("--report", type=Path, default=run / "raw/deployment-final.json")
args = parser.parse_args()
with args.report.open("x", encoding="utf-8") as output:
    json.dump(report, output, indent=2)
    output.write("\n")
print(json.dumps({"all_match": report["all_match"], "files": len(files),
                  "ready": health["ready"], "task_state": remote["task_state"],
                  "listeners": len(remote["listeners"]), "python_processes": len(remote["processes"])}))
assert report["all_match"] and health["ready"] and remote["task_state"] == "Running"
assert len(remote["listeners"]) == 1 and len(remote["processes"]) == 2
