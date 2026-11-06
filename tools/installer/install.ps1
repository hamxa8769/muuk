<#
.SYNOPSIS
  Muuk Setup: what Muuk-Setup.exe runs after unpacking itself.

.DESCRIPTION
  Installs Muuk for this Windows user (no admin rights for Muuk itself):
    1. The Visual C++ runtime, if missing or too old:
         winget install --id Microsoft.VCRedist.2015+.x64 --exact --silent ...
       or, without winget, Microsoft's vc_redist.x64.exe /install /quiet /norestart.
    2. MetaTrader 5, if the scan finds none (any broker's counts: a running one,
       MT5's data folders, the installed apps list, Program Files) and the box
       is ticked: MetaQuotes' mt5setup.exe /auto. Nothing is downloaded for
       what is already there, and anything downloaded runs only with a valid
       signature from its publisher.
    3. Muuk into %LOCALAPPDATA%\Programs\Muuk, the desktop and Start menu
       shortcuts, and an entry in Windows' installed apps list.
    4. A first config in Documents\Muuk (a demo config with no account in it).
       An existing config or preferences file is never replaced.
  A failed download doesn't stop Muuk from installing: the summary says what to
  run by hand. The parameters are for scripted installs and tests.
#>
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Muuk'),
    [string]$DesktopDir = [Environment]::GetFolderPath('DesktopDirectory'),
    [string]$StartMenuDir = [Environment]::GetFolderPath('Programs'),
    [string]$DataDir = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Muuk'),
    [string]$PrefsFile = (Join-Path $env:APPDATA 'Muuk\desktop.json'),
    [string]$UninstallKeyName = 'Muuk',
    [switch]$Silent,
    [switch]$SkipDependencies,
    [switch]$NoMetaTrader,
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$logFile = Join-Path $env:TEMP 'Muuk-Setup.log'
$script:warnings = New-Object System.Collections.ArrayList
$script:form = $null
$script:ui = @{}
$payloadFiles = 'muuk-desktop.exe', 'uninstall.ps1', 'setup-info.json', 'first-config.json'

# IExpress unpacks Setup into %TEMP%\IXP000.TMP and removes that folder on its
# own schedule -- another self-extracting installer run meanwhile (a first
# install downloads two) can take it away before Muuk is copied. So Setup
# works from its own copy, made before anything is downloaded.
$payload = Join-Path $env:TEMP ('Muuk-Setup-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $payload | Out-Null
foreach ($file in $payloadFiles + 'muuk.ico') {
    $src = Join-Path $here $file
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $payload -Force }
}
$missing = @($payloadFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $payload $_)) })
$here = $payload
$info = if (-not $missing) { Get-Content (Join-Path $here 'setup-info.json') -Raw | ConvertFrom-Json } else { [pscustomobject]@{ version = '?'; vcMinor = 0 } }

function Say([string]$text) {
    Add-Content -Path $logFile -Value ((Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $text)
    if ($Silent) { Write-Host $text; return }
    if ($script:ui.Log) {
        $script:ui.Log.AppendText($text + "`r`n")
        [System.Windows.Forms.Application]::DoEvents()
    }
}

# The line above the bar: what Setup is doing right now.
function Set-Status([string]$text) {
    if (-not $Silent -and $script:ui.Status) {
        $script:ui.Status.Text = $text
        [System.Windows.Forms.Application]::DoEvents()
    }
}

function Set-Progress([double]$percent) {
    if (-not $Silent -and $script:ui.Bar) {
        $script:ui.Bar.Value = [int][Math]::Min(100, [Math]::Max(0, $percent))
        [System.Windows.Forms.Application]::DoEvents()
    }
}

# Each step owns a stretch of the bar; a download fills its part as it goes.
$script:stepFrom = 0
$script:stepTo = 100
function Start-Step([int]$from, [int]$to, [string]$text) {
    $script:stepFrom = $from
    $script:stepTo = $to
    Set-Progress $from
    Set-Status $text
}
function Set-StepFraction([double]$fraction) {
    Set-Progress ($script:stepFrom + ($script:stepTo - $script:stepFrom) * [Math]::Min(1.0, [Math]::Max(0.0, $fraction)))
}

# Waits for a process while the window stays responsive, counting the seconds.
function Wait-Process-Ui($process, [int]$timeoutSeconds, [string]$what = '') {
    $start = Get-Date
    $deadline = $start.AddSeconds($timeoutSeconds)
    while (-not $process.HasExited) {
        if ((Get-Date) -gt $deadline) { return $false }
        if ($what) { Set-Status ("{0} ({1:N0} s)" -f $what, ((Get-Date) - $start).TotalSeconds) }
        if (-not $Silent) { [System.Windows.Forms.Application]::DoEvents() }
        Start-Sleep -Milliseconds 200
    }
    return $true
}

function Sleep-Ui([int]$milliseconds) {
    $until = (Get-Date).AddMilliseconds($milliseconds)
    while ((Get-Date) -lt $until) {
        if (-not $Silent) { [System.Windows.Forms.Application]::DoEvents() }
        Start-Sleep -Milliseconds 100
    }
}

# Downloads $url to $path with the bytes shown as they come; tries 3 times.
function Get-Download([string]$url, [string]$path, [string]$label) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    for ($try = 1; $try -le 3; $try++) {
        $response = $null; $in = $null; $out = $null
        try {
            $request = [Net.HttpWebRequest]::Create($url)
            $request.UserAgent = 'Muuk-Setup'
            # Generous: a slow line (or one busy with another download) answers late.
            $request.Timeout = 60000
            $request.ReadWriteTimeout = 120000
            $request.AllowAutoRedirect = $true
            $response = $request.GetResponse()
            $total = $response.ContentLength
            $in = $response.GetResponseStream()
            $out = [IO.File]::Create($path)
            $buffer = New-Object byte[] 65536
            $done = 0L
            $shown = [DateTime]::MinValue
            while (($n = $in.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $out.Write($buffer, 0, $n)
                $done += $n
                if (((Get-Date) - $shown).TotalMilliseconds -ge 150) {
                    $shown = Get-Date
                    if ($total -gt 0) {
                        Set-Status ("Downloading {0}: {1:N0}% ({2:N1} of {3:N1} MB)" -f $label, (100.0 * $done / $total), ($done / 1MB), ($total / 1MB))
                        Set-StepFraction ($done / $total)
                    } else {
                        Set-Status ("Downloading {0}: {1:N1} MB" -f $label, ($done / 1MB))
                    }
                }
            }
            $out.Close(); $out = $null
            if ($total -gt 0 -and $done -ne $total) { throw "the download stopped at $done of $total bytes" }
            Set-StepFraction 1.0
            return
        } catch {
            Say ("{0}: download try {1} of 3 failed: {2}" -f $label, $try, $_.Exception.Message)
            Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
            if ($try -eq 3) { throw "$label could not be downloaded: $($_.Exception.Message)" }
            Set-Status "Downloading ${label}: trying again..."
            Sleep-Ui (2000 * $try)
        } finally {
            if ($out) { $out.Close() }
            if ($in) { $in.Close() }
            if ($response) { $response.Close() }
        }
    }
}

function Test-VcRuntime {
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64') {
        $v = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($v -and $v.Installed -eq 1 -and $v.Major -eq 14 -and $v.Minor -ge [int]$info.vcMinor) { return $true }
    }
    return $false
}

# Every MetaTrader 5 on this PC, from any broker, wherever it was installed.
function Find-MetaTraderAll {
    $found = New-Object System.Collections.ArrayList
    $add = {
        param([string]$path)
        if ($path -and (Test-Path -LiteralPath $path -PathType Leaf) -and -not ($found -contains $path)) { [void]$found.Add($path) }
    }
    # Running now.
    foreach ($p in Get-Process -Name 'terminal64' -ErrorAction SilentlyContinue) { & $add $p.Path }
    # Each terminal that ever ran here names its install folder in its data folder.
    foreach ($dir in Get-ChildItem -Path (Join-Path $env:APPDATA 'MetaQuotes\Terminal') -Directory -ErrorAction SilentlyContinue) {
        $origin = Join-Path $dir.FullName 'origin.txt'
        if (Test-Path -LiteralPath $origin) {
            $folder = (Get-Content -LiteralPath $origin -Raw -ErrorAction SilentlyContinue)
            if ($folder) { & $add (Join-Path $folder.Trim() 'terminal64.exe') }
        }
    }
    # Windows' installed apps list.
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
                     'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*') {
        foreach ($app in Get-ItemProperty -Path $key -ErrorAction SilentlyContinue) {
            if ($app.DisplayName -like '*MetaTrader*' -and $app.InstallLocation) {
                & $add (Join-Path $app.InstallLocation 'terminal64.exe')
            }
        }
    }
    # The usual folders.
    foreach ($root in $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432, (Join-Path $env:LOCALAPPDATA 'Programs')) {
        if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
        foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue) {
            & $add (Join-Path $dir.FullName 'terminal64.exe')
        }
    }
    return ,$found.ToArray()
}

# Muuk finds a terminal by itself one folder under Program Files; elsewhere the config names it.
function Test-MuukFindsTerminal([string]$exe) {
    $folder = Split-Path -Parent (Split-Path -Parent $exe)
    foreach ($root in $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432) {
        if ($root -and $folder -ieq $root.TrimEnd('\')) { return $true }
    }
    return $false
}

# The one to use: a normal install first, not a copy somewhere else.
function Find-MetaTrader {
    $all = Find-MetaTraderAll
    foreach ($exe in $all) { if (Test-MuukFindsTerminal $exe) { return $exe } }
    if ($all.Count -gt 0) { return $all[0] }
    return $null
}

# Downloads to the temp folder; returns the path only with a valid signature from $publisher.
function Get-SignedDownload([string]$url, [string]$fileName, [string]$publisher, [string]$label) {
    $path = Join-Path $payload $fileName
    Get-Download $url $path $label
    Set-Status "Checking the signature of $fileName..."
    $sig = Get-AuthenticodeSignature -FilePath $path
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch $publisher) {
        Remove-Item -Path $path -Force -ErrorAction SilentlyContinue
        throw "$fileName is not validly signed by $publisher -- not running it"
    }
    return $path
}

function Install-VcRuntime {
    if (Test-VcRuntime) { Say 'Visual C++ runtime: already installed -- nothing to download.'; return }
    Say 'Visual C++ runtime: installing...'
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($winget) {
        try {
            $p = Start-Process -FilePath $winget.Source -PassThru -WindowStyle Hidden -ArgumentList @(
                'install', '--id', 'Microsoft.VCRedist.2015+.x64', '--exact', '--silent',
                '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
            [void](Wait-Process-Ui $p 600 'Visual C++ runtime: downloading and installing with winget')
        } catch { Say ('winget: ' + $_.Exception.Message) }
        if (Test-VcRuntime) { Say 'Visual C++ runtime: installed (winget).'; return }
    }
    try {
        $exe = Get-SignedDownload 'https://aka.ms/vs/17/release/vc_redist.x64.exe' 'vc_redist.x64.exe' 'Microsoft' 'the Visual C++ runtime'
        $p = Start-Process -FilePath $exe -PassThru -Verb RunAs -ArgumentList '/install', '/quiet', '/norestart'
        [void](Wait-Process-Ui $p 600 'Visual C++ runtime: installing (Windows may ask for permission)')
        Remove-Item -Path $exe -Force -ErrorAction SilentlyContinue
    } catch { Say ('vc_redist: ' + $_.Exception.Message) }
    if (Test-VcRuntime) { Say 'Visual C++ runtime: installed.'; return }
    [void]$script:warnings.Add('The Visual C++ runtime could not be installed, and Muuk needs it. Run: winget install --id Microsoft.VCRedist.2015+.x64 --exact')
    Say 'Visual C++ runtime: NOT installed.'
}

function Install-MetaTrader {
    $found = Find-MetaTrader
    if ($found) { Say "MetaTrader 5: already installed ($found) -- nothing to download."; return }
    Say 'MetaTrader 5: downloading from MetaQuotes...'
    try {
        $exe = Get-SignedDownload 'https://download.mql5.com/cdn/web/metaquotes.software.corp/mt5/mt5setup.exe' 'mt5setup.exe' 'MetaQuotes' 'MetaTrader 5'
        Say 'MetaTrader 5: installing (Windows may ask for permission)...'
        $p = Start-Process -FilePath $exe -PassThru -Verb RunAs -ArgumentList '/auto'
        $start = Get-Date
        $deadline = $start.AddMinutes(10)
        $nextScan = $start
        $found = $null
        # MetaQuotes' setup downloads the terminal itself: it can take a few minutes.
        while (-not $found -and (Get-Date) -lt $deadline -and -not $p.HasExited) {
            Set-Status ("MetaTrader 5: installing, MetaQuotes' setup downloads the terminal ({0:N0} s)" -f ((Get-Date) - $start).TotalSeconds)
            if ((Get-Date) -ge $nextScan) { $found = Find-MetaTrader; $nextScan = (Get-Date).AddSeconds(3) }
            if (-not $Silent) { [System.Windows.Forms.Application]::DoEvents() }
            Start-Sleep -Milliseconds 300
        }
    } catch { Say ('mt5setup: ' + $_.Exception.Message) }
    if (Find-MetaTrader) { Say 'MetaTrader 5: installed.'; return }
    [void]$script:warnings.Add("MetaTrader 5 could not be installed. Install your broker's MetaTrader 5 and open it once.")
    Say 'MetaTrader 5: NOT installed.'
}

function New-Shortcut([string]$path, [string]$exe) {
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = Split-Path -Parent $exe
    $lnk.IconLocation = "$exe,0"
    $lnk.Description = 'Muuk -- XAUUSD range bot'
    $lnk.Save()
}

function Write-TextFile([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($false)))
}

function Install-Muuk([bool]$desktopShortcut) {
    $exe = Join-Path $InstallDir 'muuk-desktop.exe'
    # After "Restart & install" in Muuk the app may still be closing: give it a minute.
    $start = Get-Date
    while ($true) {
        $running = Get-Process -Name 'muuk-desktop' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
        if (-not $running) { break }
        if (((Get-Date) - $start).TotalSeconds -ge 60) {
            throw 'Muuk is still running from this folder. Close it (check its open trades first), then press Retry.'
        }
        Set-Status ("Waiting for Muuk to close ({0:N0} s)" -f ((Get-Date) - $start).TotalSeconds)
        Sleep-Ui 500
    }

    Say "Muuk: copying to $InstallDir"
    Set-Status 'Muuk: copying the app...'
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    foreach ($file in 'muuk-desktop.exe', 'uninstall.ps1', 'setup-info.json') {
        # Windows can hold a just-closed exe for a moment: try again before giving up.
        for ($try = 1; ; $try++) {
            try {
                Copy-Item -LiteralPath (Join-Path $here $file) -Destination $InstallDir -Force
                break
            } catch {
                if ($try -ge 10) { throw "Can't write $file to ${InstallDir}: $($_.Exception.Message)" }
                Sleep-Ui 500
            }
        }
    }
    Set-StepFraction 0.5

    # First config and preferences: only where there are none yet.
    New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    $config = Join-Path $DataDir 'muuk.local.json'
    if (-not (Test-Path $config)) {
        $first = Get-Content -Path (Join-Path $here 'first-config.json') -Raw | ConvertFrom-Json
        $terminal = Find-MetaTrader
        if ($terminal -and -not (Test-MuukFindsTerminal $terminal)) { $first.connection.mt5.terminal = $terminal }
        Write-TextFile $config ($first | ConvertTo-Json -Depth 20)
        Say "First config: $config (a demo config -- pick your account in Muuk)"
    }
    if (-not (Test-Path $PrefsFile)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $PrefsFile) | Out-Null
        Write-TextFile $PrefsFile (@{ configPath = $config } | ConvertTo-Json)
    }

    $shortcuts = @()
    if ($desktopShortcut) { $shortcuts += (Join-Path $DesktopDir 'Muuk.lnk') }
    $shortcuts += (Join-Path $StartMenuDir 'Muuk.lnk')
    foreach ($s in $shortcuts) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $s) | Out-Null
        New-Shortcut $s $exe
        Say "Shortcut: $s"
    }

    # What the uninstaller needs, and the entry in Windows' installed apps list.
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$UninstallKeyName"
    Write-TextFile (Join-Path $InstallDir 'install-info.json') (@{ shortcuts = $shortcuts; uninstallKey = $key } | ConvertTo-Json)
    New-Item -Path $key -Force | Out-Null
    $size = [int]((Get-ChildItem -Path $InstallDir -File | Measure-Object -Property Length -Sum).Sum / 1024)
    $values = @{
        DisplayName = 'Muuk'; DisplayVersion = [string]$info.version; Publisher = 'Muuk'
        DisplayIcon = $exe; InstallLocation = $InstallDir
        UninstallString = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $InstallDir 'uninstall.ps1') + '"'
    }
    foreach ($name in $values.Keys) { New-ItemProperty -Path $key -Name $name -Value $values[$name] -PropertyType String -Force | Out-Null }
    foreach ($name in 'NoModify', 'NoRepair') { New-ItemProperty -Path $key -Name $name -Value 1 -PropertyType DWord -Force | Out-Null }
    New-ItemProperty -Path $key -Name 'EstimatedSize' -Value $size -PropertyType DWord -Force | Out-Null
    Say 'Muuk: installed.'
    return $exe
}

function Invoke-Setup([bool]$withMetaTrader, [bool]$desktopShortcut) {
    if ($missing) { throw ("Setup is incomplete ({0} missing). Download Muuk-Setup.exe again." -f ($missing -join ', ')) }
    $script:warnings.Clear()
    if (-not $SkipDependencies) {
        Start-Step 2 30 'Visual C++ runtime: checking...'
        Install-VcRuntime
        if ($withMetaTrader) {
            Start-Step 30 80 'MetaTrader 5: checking...'
            Install-MetaTrader
        } else { Say 'MetaTrader 5: skipped.' }
    }
    Start-Step 80 100 'Muuk: installing...'
    $exe = Install-Muuk $desktopShortcut
    Set-Progress 100
    Set-Status 'Done.'
    return $exe
}

function Remove-Payload {
    Remove-Item -LiteralPath $payload -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- no window
if ($Silent) {
    try {
        $exe = Invoke-Setup (-not $NoMetaTrader) $true
        foreach ($w in $script:warnings) { Write-Host "WARNING: $w" }
    } finally { Remove-Payload }
    exit 0
}

# ---------------------------------------------------------------- the window
# Its own taskbar identity, so the taskbar shows the Muuk icon, not PowerShell's.
try {
    Add-Type -Namespace MuukSetup -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int SetCurrentProcessExplicitAppUserModelID(string appId);
'@
    [void][MuukSetup.Native]::SetCurrentProcessExplicitAppUserModelID('Muuk.Setup')
} catch { Say ('taskbar id: ' + $_.Exception.Message) }
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object System.Windows.Forms.Form
$form.Text = "Muuk $($info.version) Setup"
$form.ClientSize = New-Object System.Drawing.Size(540, 400)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)
$iconFile = Join-Path $here 'muuk.ico'
if (Test-Path $iconFile) { $form.Icon = New-Object System.Drawing.Icon($iconFile) }

$title = New-Object System.Windows.Forms.Label
$title.Text = 'Install Muuk'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 14, [System.Drawing.FontStyle]::Bold)
$title.SetBounds(16, 12, 500, 32)
$intro = New-Object System.Windows.Forms.Label
$intro.Text = "Installs Muuk for this Windows user and gets what it needs. Your accounts, settings and history are never replaced.`r`nIt starts on a demo config: real money is traded only after you choose a LIVE account yourself."
$intro.SetBounds(16, 46, 508, 50)
$mt5Found = Find-MetaTrader
$mt5Box = New-Object System.Windows.Forms.CheckBox
$mt5Box.SetBounds(16, 100, 508, 22)
if ($mt5Found) {
    $mt5Box.Text = "MetaTrader 5 found, nothing to download: $mt5Found"
    $mt5Box.Checked = $false
    $mt5Box.Enabled = $false
} else {
    $mt5Box.Text = "Install MetaTrader 5 (skip it to use your broker's own)"
    $mt5Box.Checked = -not $NoMetaTrader
}
$desktopBox = New-Object System.Windows.Forms.CheckBox
$desktopBox.Text = 'Put a shortcut on the desktop'
$desktopBox.Checked = $true
$desktopBox.SetBounds(16, 124, 508, 22)
$status = New-Object System.Windows.Forms.Label
$status.Text = 'Ready to install.'
$status.AutoEllipsis = $true
$status.SetBounds(16, 150, 508, 18)
$bar = New-Object System.Windows.Forms.ProgressBar
$bar.SetBounds(16, 170, 508, 18)
$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = 'Vertical'
$log.SetBounds(16, 196, 508, 150)
$installButton = New-Object System.Windows.Forms.Button
$installButton.Text = 'Install'
$installButton.SetBounds(344, 358, 86, 30)
$closeButton = New-Object System.Windows.Forms.Button
$closeButton.Text = 'Cancel'
$closeButton.SetBounds(438, 358, 86, 30)
$form.Controls.AddRange(@($title, $intro, $mt5Box, $desktopBox, $status, $bar, $log, $installButton, $closeButton))
$script:ui = @{ Log = $log; Bar = $bar; Status = $status }
if ($missing) {
    Say ("Setup is incomplete ({0} missing). Download Muuk-Setup.exe again." -f ($missing -join ', '))
    $installButton.Enabled = $false
}

$script:installedExe = $null
$script:busy = $false
$closeButton.Add_Click({ $form.Close() })
$form.Add_FormClosing({ param($s, $e) if ($script:busy) { $e.Cancel = $true } })
$installButton.Add_Click({
    if ($script:installedExe) {
        Start-Process -FilePath $script:installedExe -WorkingDirectory (Split-Path -Parent $script:installedExe)
        $form.Close()
        return
    }
    $script:busy = $true
    $installButton.Enabled = $false
    $closeButton.Enabled = $false
    $mt5Box.Enabled = $false
    $desktopBox.Enabled = $false
    try {
        $script:installedExe = Invoke-Setup $mt5Box.Checked $desktopBox.Checked
        if ($script:warnings.Count -gt 0) {
            Say ''
            foreach ($w in $script:warnings) { Say "WARNING: $w" }
        }
        Say ''
        Say 'Done. Open Muuk from the shortcut, then pick your account with the Accounts button.'
        $installButton.Text = 'Open Muuk'
        $installButton.Enabled = -not $NoLaunch
        $closeButton.Text = 'Close'
    } catch {
        Say ''
        Say ('Setup failed: ' + $_.Exception.Message)
        Say "Details: $logFile"
        Say 'Press Retry: what is already installed is not downloaded again.'
        Set-Status ('Failed: ' + $_.Exception.Message)
        $installButton.Text = 'Retry'
        $installButton.Enabled = $true
        $mt5Box.Enabled = -not $mt5Found
        $desktopBox.Enabled = $true
        $closeButton.Text = 'Close'
    }
    $closeButton.Enabled = $true
    $script:busy = $false
})
try { [void]$form.ShowDialog() } finally { Remove-Payload }
