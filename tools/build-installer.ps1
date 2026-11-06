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

# IExpress gives Setup its own generic icon: put Muuk's in its place. Every
# icon group in the exe (each language) gets Muuk's images; the old images stay
# unused. IExpress keeps its payload in resources too, which this leaves alone.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

public static class MuukIcon {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    delegate bool EnumNames(IntPtr module, IntPtr type, IntPtr name, IntPtr param);
    delegate bool EnumLangs(IntPtr module, IntPtr type, IntPtr name, ushort lang, IntPtr param);
    [DllImport("kernel32.dll")] static extern bool EnumResourceNames(IntPtr module, IntPtr type, EnumNames cb, IntPtr param);
    [DllImport("kernel32.dll")] static extern bool EnumResourceLanguages(IntPtr module, IntPtr type, IntPtr name, EnumLangs cb, IntPtr param);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateResource(IntPtr update, IntPtr type, IntPtr name, ushort lang, byte[] data, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool EndUpdateResource(IntPtr update, bool discard);

    const int RT_ICON = 3, RT_GROUP_ICON = 14;

    public static int Replace(string exe, string ico) {
        byte[] icon = File.ReadAllBytes(ico);
        int count = BitConverter.ToUInt16(icon, 4);
        // The exe's icon groups, by name (an id, or a string kept as IntPtr) and language.
        var groups = new List<KeyValuePair<IntPtr, ushort>>();
        var names = new List<string>();
        IntPtr module = LoadLibraryEx(exe, IntPtr.Zero, 0x2 /* LOAD_LIBRARY_AS_DATAFILE */);
        if (module == IntPtr.Zero) throw new Exception("can't open " + exe);
        try {
            EnumResourceNames(module, (IntPtr)RT_GROUP_ICON, (m, t, n, p) => {
                IntPtr key = ((long)n >> 16) == 0 ? n : Marshal.StringToHGlobalUni(Marshal.PtrToStringUni(n));
                EnumResourceLanguages(m, t, n, (m2, t2, n2, lang, p2) => {
                    groups.Add(new KeyValuePair<IntPtr, ushort>(key, lang));
                    return true;
                }, IntPtr.Zero);
                return true;
            }, IntPtr.Zero);
        } finally { FreeLibrary(module); }
        if (groups.Count == 0) groups.Add(new KeyValuePair<IntPtr, ushort>((IntPtr)1, 1033));

        IntPtr update = BeginUpdateResource(exe, false);
        if (update == IntPtr.Zero) throw new Exception("can't update " + exe);
        bool ok = false;
        try {
            const int firstId = 9100;  // past IExpress's own icon ids
            var langs = new HashSet<ushort>();
            foreach (var g in groups) langs.Add(g.Value);
            // The images, once per language used by a group.
            foreach (ushort lang in langs) {
                for (int i = 0; i < count; i++) {
                    int e = 6 + 16 * i;
                    int size = BitConverter.ToInt32(icon, e + 8);
                    int offset = BitConverter.ToInt32(icon, e + 12);
                    byte[] image = new byte[size];
                    Array.Copy(icon, offset, image, 0, size);
                    if (!UpdateResource(update, (IntPtr)RT_ICON, (IntPtr)(firstId + i), lang, image, (uint)size))
                        throw new Exception("icon image " + i + ": error " + Marshal.GetLastWin32Error());
                }
            }
            // The group: the .ico's header, with each entry's file offset replaced by its resource id.
            byte[] group = new byte[6 + 14 * count];
            Array.Copy(icon, 0, group, 0, 6);
            for (int i = 0; i < count; i++) {
                Array.Copy(icon, 6 + 16 * i, group, 6 + 14 * i, 12);
                BitConverter.GetBytes((ushort)(firstId + i)).CopyTo(group, 6 + 14 * i + 12);
            }
            foreach (var g in groups) {
                if (!UpdateResource(update, (IntPtr)RT_GROUP_ICON, g.Key, g.Value, group, (uint)group.Length))
                    throw new Exception("icon group: error " + Marshal.GetLastWin32Error());
            }
            ok = true;
        } finally {
            if (!EndUpdateResource(update, !ok) && ok) throw new Exception("saving the icon: error " + Marshal.GetLastWin32Error());
        }
        return groups.Count;
    }
}
'@
$groups = [MuukIcon]::Replace($Out, (Join-Path $root 'app\desktop\res\muuk.ico'))
Write-Output "Setup icon: Muuk's ($groups icon group(s) replaced)"
Write-Output ("Built {0} ({1:N1} MB), Muuk {2}" -f $Out, ((Get-Item $Out).Length / 1MB), $version)
