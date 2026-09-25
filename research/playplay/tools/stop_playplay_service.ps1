param([string]$TaskName = 'CspotPlayPlayDfa')
$ErrorActionPreference = 'Stop'
$task = Get-ScheduledTask -TaskName $TaskName
if ($task.State -ne 'Running') {
    Write-Output 'Service task is already stopped.'
    exit 0
}
# Let Python unload Frida and detach before the task exits. Do not kill Spotify.
New-Item -ItemType File -Path (Join-Path $PSScriptRoot 'playplay-service.stop') -Force | Out-Null
$deadline = (Get-Date).AddSeconds(30)
do {
    Start-Sleep -Milliseconds 250
    $task = Get-ScheduledTask -TaskName $TaskName
    if ($task.State -ne 'Running') {
        Write-Output 'Service stopped and Frida detached.'
        exit 0
    }
} while ((Get-Date) -lt $deadline)
throw 'Service did not stop within 30 seconds; inspect it before forcing termination.'
