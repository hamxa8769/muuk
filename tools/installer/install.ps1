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
$info = Get-Content (Join-Path $here 'setup-info.json') -Raw | ConvertFrom-Json
$logFile = Join-Path $env:TEMP 'Muuk-Setup.log'
$script:warnings = New-Object System.Collections.ArrayList
$script:form = $null
$script:ui = @{}

function Say([string]$text) {
    Add-Content -Path $logFile -Value ((Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $text)
    if ($Silent) { Write-Host $text; return }
    if ($script:ui.Log) {
        $script:ui.Log.AppendText($text + "`r`n")
        [System.Windows.Forms.Application]::DoEvents()
    }
}

function Set-Progress([int]$percent) {
    if (-not $Silent -and $script:ui.Bar) {
        $script:ui.Bar.Value = [Math]::Min(100, [Math]::Max(0, $percent))
        [System.Windows.Forms.Application]::DoEvents()
    }
}

# Waits for a process while the window stays responsive.
function Wait-Process-Ui($process, [int]$timeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while (-not $process.HasExited) {
        if ((Get-Date) -gt $deadline) { return $false }
        if (-not $Silent) { [System.Windows.Forms.Application]::DoEvents() }
        Start-Sleep -Milliseconds 200
    }
    return $true
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
function Get-SignedDownload([string]$url, [string]$fileName, [string]$publisher) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $path = Join-Path $env:TEMP $fileName
    $web = New-Object Net.WebClient
    $task = $web.DownloadFileTaskAsync($url, $path)
    while (-not $task.IsCompleted) {
        if (-not $Silent) { [System.Windows.Forms.Application]::DoEvents() }
        Start-Sleep -Milliseconds 200
    }
    $web.Dispose()
    if ($task.IsFaulted) { throw $task.Exception.InnerException }
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
            [void](Wait-Process-Ui $p 600)
        } catch { Say ('winget: ' + $_.Exception.Message) }
        if (Test-VcRuntime) { Say 'Visual C++ runtime: installed (winget).'; return }
    }
    try {
        $exe = Get-SignedDownload 'https://aka.ms/vs/17/release/vc_redist.x64.exe' 'vc_redist.x64.exe' 'Microsoft'
        $p = Start-Process -FilePath $exe -PassThru -Verb RunAs -ArgumentList '/install', '/quiet', '/norestart'
        [void](Wait-Process-Ui $p 600)
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
        $exe = Get-SignedDownload 'https://download.mql5.com/cdn/web/metaquotes.software.corp/mt5/mt5setup.exe' 'mt5setup.exe' 'MetaQuotes'
        Say 'MetaTrader 5: installing (Windows may ask for permission)...'
        $p = Start-Process -FilePath $exe -PassThru -Verb RunAs -ArgumentList '/auto'
        $deadline = (Get-Date).AddMinutes(10)
        while (-not (Find-MetaTrader) -and (Get-Date) -lt $deadline -and -not $p.HasExited) {
            if (-not $Silent) { [System.Windows.Forms.Application]::DoEvents() }
            Start-Sleep -Milliseconds 500
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
    $running = Get-Process -Name 'muuk-desktop' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
    if ($running) { throw 'Muuk is running from this folder. Close it (check its open trades first) and run Setup again.' }

    Say "Muuk: copying to $InstallDir"
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    foreach ($file in 'muuk-desktop.exe', 'uninstall.ps1', 'setup-info.json') {
        Copy-Item -Path (Join-Path $here $file) -Destination $InstallDir -Force
    }

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
    if (-not $SkipDependencies) {
        Set-Progress 5
        Install-VcRuntime
        Set-Progress 35
        if ($withMetaTrader) { Install-MetaTrader } else { Say 'MetaTrader 5: skipped.' }
    }
    Set-Progress 75
    $exe = Install-Muuk $desktopShortcut
    Set-Progress 100
    return $exe
}

# ---------------------------------------------------------------- no window
if ($Silent) {
    $exe = Invoke-Setup (-not $NoMetaTrader) $true
    foreach ($w in $script:warnings) { Write-Host "WARNING: $w" }
    exit 0
}

# ---------------------------------------------------------------- the window
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
$bar = New-Object System.Windows.Forms.ProgressBar
$bar.SetBounds(16, 154, 508, 18)
$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = 'Vertical'
$log.SetBounds(16, 180, 508, 166)
$installButton = New-Object System.Windows.Forms.Button
$installButton.Text = 'Install'
$installButton.SetBounds(344, 358, 86, 30)
$closeButton = New-Object System.Windows.Forms.Button
$closeButton.Text = 'Cancel'
$closeButton.SetBounds(438, 358, 86, 30)
$form.Controls.AddRange(@($title, $intro, $mt5Box, $desktopBox, $bar, $log, $installButton, $closeButton))
$script:ui = @{ Log = $log; Bar = $bar }

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
        $closeButton.Text = 'Close'
    }
    $closeButton.Enabled = $true
    $script:busy = $false
})
[void]$form.ShowDialog()
