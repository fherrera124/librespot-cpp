param(
    [int]$Port = 8765,
    [string]$Python = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
)
$ErrorActionPreference = 'Stop'
$mainProcesses = @(Get-CimInstance Win32_Process -Filter "Name='Spotify.exe'" |
    Where-Object { $_.CommandLine -and $_.CommandLine -notmatch '--type=' })
if ($mainProcesses.Count -ne 1) {
    throw 'Expected exactly one running main Spotify.exe process.'
}
$spotifyProcessId = $mainProcesses[0].ProcessId
& $Python -B (Join-Path $PSScriptRoot 'playplay_dfa_service.py') `
    --pid $spotifyProcessId --host 127.0.0.1 --port $Port `
    --allow-unauthenticated-loopback
exit $LASTEXITCODE
