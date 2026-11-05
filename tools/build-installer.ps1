<#
.SYNOPSIS
  Builds dist\Muuk-Setup.exe, the Muuk installer, with Windows' own IExpress
  (no extra tool to install). It never packs a *.local.json, a state file or
  anything else of yours: the first config comes from the example config with
  the account fields emptied.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\build-installer.ps1
#>
param([string]$Out = "")

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'dist\Muuk-Setup.exe' }
$stage = Join-Path $root 'build\installer-stage'
$exe = Join-Path $root 'build\app\desktop\Release\muuk-desktop.exe'

& cmake --build (Join-Path $root 'build') --config Release --target muuk-desktop -- /m /v:q
if ($LASTEXITCODE -ne 0) { throw 'The Release build failed (is Muuk still running?)' }

if (Test-Path $stage) { Remove-Item -Path $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Split-Path -Parent $Out) | Out-Null
Copy-Item -Path $exe -Destination $stage
Copy-Item -Path (Join-Path $root 'app\desktop\res\muuk.ico') -Destination $stage
Copy-Item -Path (Join-Path $PSScriptRoot 'installer\install.ps1'), (Join-Path $PSScriptRoot 'installer\uninstall.ps1') -Destination $stage

# The first config: the example, on a demo, with no account in it.
$config = Get-Content (Join-Path $root 'engine\ctrader_fix\config.mt5.example.json') -Raw | ConvertFrom-Json
$config.connection.environment = 'demo'
$config.connection.mt5.login = ''
$config.connection.mt5.server = ''
$config.connection.mt5.PSObject.Properties.Remove('password')
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $stage 'first-config.json'), ($config | ConvertTo-Json -Depth 20), $utf8)

# The app needs the Visual C++ runtime of the toolset that built it (14.<minor>).
$linker = (Select-String -Path (Join-Path $root 'build\CMakeCache.txt') -Pattern '^CMAKE_LINKER:').Line
if ($linker -notmatch '/MSVC/14\.(\d+)\.') { throw "Can't read the MSVC version from: $linker" }
$version = (Get-Item $exe).VersionInfo.ProductVersion
[IO.File]::WriteAllText((Join-Path $stage 'setup-info.json'),
    (@{ version = $version; vcMinor = [int]$Matches[1] } | ConvertTo-Json), $utf8)

# Nothing of the user's may go in, whatever the folder holds.
$files = Get-ChildItem -Path $stage -File
if ($files | Where-Object { $_.Name -like '*.local.json' -or $_.Name -like '*.state.json' }) { throw 'A private file is in the installer stage' }

$lines = @(
    '[Version]', 'Class=IEXPRESS', 'SEDVersion=3',
    '[Options]', 'PackagePurpose=InstallApp', 'ShowInstallProgramWindow=1', 'HideExtractAnimation=1',
    'UseLongFileName=1', 'InsideCompressed=0', 'CAB_FixedSize=0', 'CAB_ResvCodeSigning=0', 'RebootMode=N',
    'InstallPrompt=%InstallPrompt%', 'DisplayLicense=%DisplayLicense%', 'FinishMessage=%FinishMessage%',
    'TargetName=%TargetName%', 'FriendlyName=%FriendlyName%', 'AppLaunched=%AppLaunched%',
    'PostInstallCmd=%PostInstallCmd%', 'AdminQuietInstCmd=%AdminQuietInstCmd%', 'UserQuietInstCmd=%UserQuietInstCmd%',
    'SourceFiles=SourceFiles',
    '[Strings]', 'InstallPrompt=', 'DisplayLicense=', 'FinishMessage=', "TargetName=$Out", 'FriendlyName=Muuk Setup',
    'AppLaunched=powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File install.ps1',
    'PostInstallCmd=<None>', 'AdminQuietInstCmd=', 'UserQuietInstCmd='
)
for ($i = 0; $i -lt $files.Count; $i++) { $lines += "FILE$i=`"$($files[$i].Name)`"" }
$lines += '[SourceFiles]', "SourceFiles0=$stage\", '[SourceFiles0]'
for ($i = 0; $i -lt $files.Count; $i++) { $lines += "%FILE$i%=" }
$sed = Join-Path $root 'build\muuk-setup.sed'
Set-Content -Path $sed -Value $lines -Encoding ASCII

if (Test-Path $Out) { Remove-Item -Path $Out -Force }
Start-Process -FilePath "$env:WINDIR\System32\iexpress.exe" -ArgumentList '/N', '/Q', $sed -Wait -WindowStyle Hidden
if (-not (Test-Path $Out)) { throw 'IExpress did not write the installer' }
Write-Output ("Built {0} ({1:N1} MB), Muuk {2}" -f $Out, ((Get-Item $Out).Length / 1MB), $version)
