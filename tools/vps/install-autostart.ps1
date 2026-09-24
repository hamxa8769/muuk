<#
.SYNOPSIS
  Makes Muuk start by itself on this VPS (Windows Task Scheduler).

.DESCRIPTION
  -Mode desktop (default): starts muuk-desktop.exe when you log on, connected
      (--connect), restarted if it stops. Needs a logged-on session -- for a
      VPS that reboots by itself, also turn on automatic logon.
  -Mode live: starts the console bot muuk-live.exe at logon, restarted if it
      stops. (It reconnects by itself too.)
  -Remove: deletes the task.
  Run from the folder that holds the exes and your config.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1 -Config .\ic_markets_demo.local.json
  powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1 -Mode live -Config .\ic_markets_demo.local.json
  powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1 -Remove
#>
param(
    [ValidateSet("desktop", "live")][string]$Mode = "desktop",
    [string]$Config = "",
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$taskName = "Muuk bot"

if ($Remove) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Removed the '$taskName' task."
    return
}

if (-not $Config) {
    $Config = Get-ChildItem $here -Filter "*.local.json" | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Config -or -not (Test-Path $Config)) { throw "Config file not found -- pass -Config <file>" }
$Config = (Resolve-Path $Config).Path

if ($Mode -eq "desktop") {
    $exe = Join-Path $here "muuk-desktop.exe"
    $arguments = "--config `"$Config`" --connect"
} else {
    $exe = Join-Path $here "muuk-live.exe"
    $arguments = "--credentials `"$Config`""
}
if (-not (Test-Path $exe)) { throw "$exe not found -- run this from the Muuk folder" }

$action = New-ScheduledTaskAction -Execute $exe -Argument $arguments -WorkingDirectory $here
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings `
    -Principal $principal -Force | Out-Null

Write-Host "Task '$taskName' installed: $([IO.Path]::GetFileName($exe)) starts at logon and restarts if it stops."
Write-Host "Try it now:  Start-ScheduledTask -TaskName '$taskName'"
Write-Host "If the VPS may reboot on its own (Windows Update), turn on automatic logon for this user"
Write-Host "(Sysinternals Autologon), otherwise nothing logs on and the task never starts."
