<#
.SYNOPSIS
  Removes Muuk: its program folder, shortcuts and installed-apps entry.
  Documents\Muuk (configs, history, reports), MetaTrader 5 and the Visual C++
  runtime stay. Open trades stay at the broker.
#>
param([switch]$Silent)

$ErrorActionPreference = 'Stop'
$installDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$exe = Join-Path $installDir 'muuk-desktop.exe'

function Tell([string]$text, [string]$buttons = 'OK') {
    if ($Silent) { Write-Host $text; return 'Yes' }
    Add-Type -AssemblyName System.Windows.Forms
    return [string][System.Windows.Forms.MessageBox]::Show($text, 'Uninstall Muuk', $buttons)
}

if (Get-Process -Name 'muuk-desktop' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
    [void](Tell 'Muuk is running. Close it first (check its open trades), then uninstall again.')
    exit 1
}
if ((Tell "Remove Muuk from this PC?`r`n`r`nYour configs, history and reports in Documents\Muuk stay, and so does MetaTrader 5. Any open trades stay open at the broker." 'YesNo') -ne 'Yes') {
    exit 0
}

$info = Get-Content (Join-Path $installDir 'install-info.json') -Raw | ConvertFrom-Json
$shell = New-Object -ComObject WScript.Shell
foreach ($path in $info.shortcuts) {
    # Only a shortcut to this very program (not another copy's).
    if ((Test-Path $path) -and $shell.CreateShortcut($path).TargetPath -eq $exe) {
        Remove-Item -Path $path -Force
    }
}
# "Start Muuk with Windows", if it starts this copy.
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$value = (Get-ItemProperty -Path $run -Name 'Muuk' -ErrorAction SilentlyContinue).Muuk
if ($value -and $value -like "*$exe*") { Remove-ItemProperty -Path $run -Name 'Muuk' }

if (Test-Path $info.uninstallKey) { Remove-Item -Path $info.uninstallKey -Recurse -Force }
Set-Location -Path $env:TEMP
Remove-Item -Path $installDir -Recurse -Force
[void](Tell 'Muuk was removed. Your data is still in Documents\Muuk.')
