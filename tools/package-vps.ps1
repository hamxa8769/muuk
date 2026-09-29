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
  keep the folder and zip private). With -WithData also the basket history
  (muuk_history) and the saved running basket (muuk_state) for that config,
  so the VPS carries on with the same data -- stop the bot on this PC first.
  The output folder is recreated from scratch every time.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package-vps.ps1 -Config .\ic_markets_demo.local.json -WithData
#>
param(
    [string]$Config = "",
    [string]$Out = "",
    [switch]$WithData,
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

# Start from an empty folder: nothing left over from an earlier package.
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
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

# MetaTrader 5 bridge: the EA's source, compiled with MetaEditor when this PC has it.
$mt5Out = Join-Path $Out "mt5"
New-Item -ItemType Directory -Force $mt5Out | Out-Null
Copy-Item (Join-Path $PSScriptRoot "mt5\MuukBridge.mq5") $mt5Out -Force
Copy-Item (Join-Path $repo "docs\mt5.md") (Join-Path $Out "README-MT5.md") -Force
Copy-Item (Join-Path $repo "engine\ctrader_fix\config.mt5.example.json") (Join-Path $mt5Out "mt5_account.example.json") -Force
$metaEditor = Get-ChildItem "C:\Program Files\*\metaeditor64.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($metaEditor) {
    $log = Join-Path $env:TEMP "muukbridge_compile.log"
    $p = Start-Process -FilePath $metaEditor.FullName -ArgumentList "/compile:`"$mt5Out\MuukBridge.mq5`"", "/log:`"$log`"" -Wait -PassThru -WindowStyle Hidden
    if (Test-Path (Join-Path $mt5Out "MuukBridge.ex5")) {
        Write-Host "MuukBridge.ex5 compiled with $($metaEditor.FullName)"
    } else {
        Write-Warning "MuukBridge didn't compile -- see $log. The .mq5 is in the package; compile it in MetaEditor."
    }
} else {
    Write-Warning "MetaEditor not found: only MuukBridge.mq5 is packaged -- open it in MetaEditor (F4 in MT5) and press Compile."
}

if ($Config) {
    $cfg = Resolve-Path $Config
    Copy-Item $cfg $Out -Force
    Write-Warning "Copied $(Split-Path $cfg -Leaf) -- it contains your FIX password. Keep the folder/zip private."
    if ($WithData) {
        if (Get-Process muuk-desktop, muuk-live -ErrorAction SilentlyContinue) {
            Write-Warning "Muuk is running here: the copied state may already be out of date. Stop it, then package again."
        }
        $dataDir = Split-Path $cfg -Parent
        # One history file per account file (older versions shared baskets.jsonl).
        $stem = [IO.Path]::GetFileNameWithoutExtension($cfg)
        $history = Join-Path (Join-Path $dataDir "muuk_history") "$stem.baskets.jsonl"
        if (-not (Test-Path $history)) { $history = Join-Path (Join-Path $dataDir "muuk_history") "baskets.jsonl" }
        if (Test-Path $history) {
            New-Item -ItemType Directory -Force (Join-Path $Out "muuk_history") | Out-Null
            Copy-Item $history (Join-Path $Out "muuk_history") -Force
            Write-Host "Basket history copied ($((Get-Content $history | Measure-Object -Line).Lines) baskets)."
        }
        $stateName = [IO.Path]::GetFileNameWithoutExtension($cfg) + ".state.json"
        $state = Join-Path $dataDir "muuk_state\$stateName"
        if (Test-Path $state) {
            New-Item -ItemType Directory -Force (Join-Path $Out "muuk_state") | Out-Null
            Copy-Item $state (Join-Path $Out "muuk_state") -Force
            Write-Host "Saved state copied ($stateName)."
        }
    }
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
