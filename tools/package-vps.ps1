<#
.SYNOPSIS
  Builds a ready-to-copy folder (and zip) for running Muuk on a Windows VPS.

.DESCRIPTION
  Collects, into dist\Muuk-VPS:
    muuk-desktop.exe, muuk-live.exe      the app and the console bot (Release build)
    msvcp140.dll, vcruntime140*.dll      the Visual C++ runtime, so the VPS needs no installer
    check-vps.ps1                        checks the VPS is fit to trade (latency, clock, disk, power)
    install-autostart.ps1                starts the bot automatically at logon / startup
    start-desktop.cmd, start-live.cmd    double-click starters
    README-VPS.md                        the guide (docs\vps.md)
  and your config file if you pass -Config (it holds your FIX password --
  keep the folder and zip private).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package-vps.ps1 -Config .\ic_markets_demo.local.json
#>
param(
    [string]$Config = "",
    [string]$Out = "",
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo "dist\Muuk-VPS" }

$desktop = Join-Path $repo "build\app\desktop\Release\muuk-desktop.exe"
$live = Join-Path $repo "build\engine\ctrader_fix\Release\muuk-live.exe"
foreach ($exe in @($desktop, $live)) {
    if (-not (Test-Path $exe)) { throw "Missing $exe -- build Release first: cmake --build build --config Release" }
}
if (Get-Process muuk-desktop, muuk-live -ErrorAction SilentlyContinue) {
    Write-Warning "Muuk is running on this PC -- that's fine, the files are only copied."
}

# Visual C++ runtime, copied next to the exes (app-local), from the Build Tools' redist folder.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$crt = $null
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -property installationPath
    $crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
}
$crtFiles = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")

New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item $desktop, $live $Out -Force
if ($crt) {
    foreach ($f in $crtFiles) { Copy-Item (Join-Path $crt.FullName $f) $Out -Force }
    Write-Host "Visual C++ runtime: $($crt.FullName)"
} else {
    Write-Warning "Visual C++ redist folder not found -- install 'Microsoft Visual C++ Redistributable (x64)' on the VPS."
}
Copy-Item (Join-Path $PSScriptRoot "vps\*") $Out -Force
Copy-Item (Join-Path $repo "docs\vps.md") (Join-Path $Out "README-VPS.md") -Force

if ($Config) {
    $cfg = Resolve-Path $Config
    Copy-Item $cfg $Out -Force
    Write-Warning "Copied $(Split-Path $cfg -Leaf) -- it contains your FIX password. Keep the folder/zip private."
} else {
    Copy-Item (Join-Path $repo "engine\ctrader_fix\config.example.json") (Join-Path $Out "my_account.local.json") -Force
    Write-Host "No -Config given: put your real config in the folder (a template my_account.local.json is there)."
}

if (-not $NoZip) {
    $zip = "$Out.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path (Join-Path $Out "*") -DestinationPath $zip
    Write-Host "Zip: $zip"
}
Write-Host "Folder: $Out"
Get-ChildItem $Out | Select-Object Name, @{ n = "KB"; e = { [math]::Round($_.Length / 1KB) } } | Format-Table -AutoSize
