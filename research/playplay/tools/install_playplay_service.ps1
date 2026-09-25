# Register an on-demand task in the logged-in user's session. Start Spotify first.
param([string]$TaskName = 'CspotPlayPlayDfa')
$ErrorActionPreference = 'Stop'
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw "Task $TaskName already exists; inspect it before replacing it."
}
$userName = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$python = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
$server = Join-Path $PSScriptRoot 'playplay_dfa_service.py'
$action = New-ScheduledTaskAction -Execute $python `
    -Argument ('-B "{0}" --allow-unauthenticated-loopback' -f $server) `
    -WorkingDirectory $PSScriptRoot
$principal = New-ScheduledTaskPrincipal -UserId $userName -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal `
    -Settings $settings -Description 'Local PlayPlay HTTP API for cspot; start after Spotify.' | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Output "Started task $TaskName for $userName"
