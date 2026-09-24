<#
.SYNOPSIS
  Checks that this machine is fit to run the Muuk bot. Changes nothing.

.DESCRIPTION
  - Network: TCP connect time to the broker's trade and quote servers (from
    the config file) -- no login, nothing is sent.
  - Clock: Windows time sync status (FIX messages carry timestamps).
  - Power: the active plan (sleep would stop the bot).
  - Disk, memory, CPU cores, Windows version, Visual C++ runtime next to the exes.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\check-vps.ps1 -Config .\ic_markets_demo.local.json
#>
param([string]$Config = "")

$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
function Report([string]$Name, [string]$Status, [string]$Detail) {
    $color = switch ($Status) { "OK" { "Green" } "WARN" { "Yellow" } default { "Red" } }
    Write-Host ("{0,-22} " -f $Name) -NoNewline
    Write-Host ("{0,-5}" -f $Status) -ForegroundColor $color -NoNewline
    Write-Host " $Detail"
}

if (-not $Config) {
    $Config = Get-ChildItem $here -Filter "*.local.json" | Select-Object -First 1 -ExpandProperty FullName
}

Write-Host "Muuk VPS check -- $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Cyan
Write-Host ""

# Windows and hardware
$os = Get-CimInstance Win32_OperatingSystem
$cs = Get-CimInstance Win32_ComputerSystem
Report "Windows" "OK" "$($os.Caption) (build $($os.BuildNumber))"
$cores = $cs.NumberOfLogicalProcessors
Report "CPU cores" ($(if ($cores -ge 2) { "OK" } else { "WARN" })) "$cores"
$totalGb = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
$freeGb = [math]::Round($os.FreePhysicalMemory * 1KB / 1GB, 1)
Report "Memory" ($(if ($totalGb -ge 2) { "OK" } else { "WARN" })) "$freeGb GB free of $totalGb GB"
$drive = (Get-Item $here).PSDrive
$diskFree = [math]::Round($drive.Free / 1GB, 1)
Report "Disk ($($drive.Name):)" ($(if ($diskFree -ge 2) { "OK" } else { "FAIL" })) "$diskFree GB free"

# Runtime next to the exes
$missing = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") | Where-Object {
    -not (Test-Path (Join-Path $here $_)) -and -not (Test-Path (Join-Path $env:WINDIR "System32\$_"))
}
Report "VC++ runtime" ($(if ($missing) { "FAIL" } else { "OK" })) ($(if ($missing) { "missing: $($missing -join ', ')" } else { "found" }))

# Power: sleep or hibernate stops the bot
$scheme = (powercfg /getactivescheme) -join " "
$sleepAc = (powercfg /query SCHEME_CURRENT SUB_SLEEP STANDBYIDLE 2>$null | Select-String "Current AC Power Setting Index" | Select-Object -First 1)
$sleepSecs = if ($sleepAc) { [Convert]::ToInt32(($sleepAc.ToString() -split ":")[-1].Trim(), 16) } else { -1 }
Report "Power plan" ($(if ($sleepSecs -eq 0) { "OK" } else { "WARN" })) ("$($scheme -replace '^.*\((.*)\).*$', '$1'); sleep after " +
    $(if ($sleepSecs -eq 0) { "never" } elseif ($sleepSecs -gt 0) { "$([math]::Round($sleepSecs / 60)) min -- set it to Never" } else { "unknown" }))

# Clock
$w32 = (w32tm /query /status 2>$null) -join "`n"
if ($w32 -match "Last Successful Sync Time:\s*(.+)") {
    Report "Time sync" "OK" "last sync $($Matches[1].Trim())"
} else {
    Report "Time sync" "WARN" "not confirmed -- Settings > Time & language > Date & time > Sync now"
}

# Broker network latency (TCP connect only -- no login)
if ($Config -and (Test-Path $Config)) {
    $cfg = Get-Content $Config -Raw | ConvertFrom-Json
    foreach ($name in @("trade", "quote")) {
        $ep = $cfg.connection.$name
        if (-not $ep) { continue }
        $times = @()
        for ($i = 0; $i -lt 5; $i++) {
            $client = New-Object System.Net.Sockets.TcpClient
            $sw = [Diagnostics.Stopwatch]::StartNew()
            try {
                $task = $client.ConnectAsync($ep.host, [int]$ep.port)
                if ($task.Wait(3000) -and $client.Connected) { $times += $sw.Elapsed.TotalMilliseconds }
            } catch { }
            $client.Close()
            Start-Sleep -Milliseconds 200
        }
        if ($times.Count -eq 0) {
            Report "Broker $name" "FAIL" "can't reach $($ep.host):$($ep.port) (firewall? wrong host?)"
        } else {
            $avg = [math]::Round(($times | Measure-Object -Average).Average, 1)
            $st = if ($avg -le 20) { "OK" } elseif ($avg -le 100) { "WARN" } else { "FAIL" }
            Report "Broker $name" $st "$avg ms to connect ($($ep.host):$($ep.port)) -- under 20 ms is ideal"
        }
    }
} else {
    Report "Broker latency" "WARN" "no config found -- pass -Config <file>"
}

Write-Host ""
Write-Host "Next: start the app (start-desktop.cmd), connect, and watch the Performance tab for a few minutes."
