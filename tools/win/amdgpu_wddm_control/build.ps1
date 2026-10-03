# Builds the amdgpu-wddm control application: amdgpu_wddm_control.exe (.NET Framework 4.8, part of Windows 10/11,
# nothing to install on a tester's PC) and bc250control.dll (tools/win/bc250kmd_cli/bc250kmd_cli.c with
# BC250_CONTROL_DLL), and bc250kmd_cli.exe from the same file for the release's tools folder. Compilers from the installed Visual Studio, headers and import libraries from the SDK NuGet
# packages under -Kits. Deterministic: the same sources give the same bytes (csc /deterministic, cl/link /Brepro).
#
#   pwsh tools\win\amdgpu_wddm_control\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\release\control-app\build\app
#        [-StartConfirmCore <release installer's start-confirm-core.ps1>]
#
# Gates, in order, each stops the build: the unit tests (test/UnitTests.cs against driver/kmd/bc250kmd_escape.h, the
# KMD's interop, guard and DPM sources, the D3D12 shell's switch names, and with -StartConfirmCore the installer's
# confirmation rule), the DLL and app compiles with warnings as errors, a smoke run of the exe with --smoke (no
# window: the pages are built and refreshed once, their text written to smoke.txt), the bug report smoke, and the
# Recovery dry runs (--action X --dry-run: nothing is written, no UAC) against test/snapshot-bd059.json and this PC,
# and --smoke-render at 96, 120 and 144 DPI (every page drawn to a PNG, no window; no two controls may overlap).
# The smoke run passes on a PC without a BC-250 when it reports the driver as not found.

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [string]$StartConfirmCore,
    [switch]$NoSmoke
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null

$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.IO.Compression.dll', 'System.Management.dll', 'System.Web.Extensions.dll' |
    ForEach-Object { "/reference:$fx\$_" }
$pure = 'KmdReply.cs', 'Profiles.cs', 'Redactor.cs', 'ManifestCheck.cs', 'Recovery.cs' | ForEach-Object { Join-Path $here "src\$_" }

# 1. Unit tests of the pure parts.
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ /langversion:7.3 /deterministic+ `
    "/out:$obj\unit-tests.exe" @pure (Join-Path $here 'test\UnitTests.cs')
if ($LASTEXITCODE -ne 0) { throw "unit test compile failed ($LASTEXITCODE)" }
$unitArgs = @($repo) + @(if ($StartConfirmCore) { (Resolve-Path $StartConfirmCore).Path })
& "$obj\unit-tests.exe" @unitArgs | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'unit tests failed' }

# 2. bc250control.dll, the same translation unit and flags as tools\win\bc250kmd_cli\build.ps1.
$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/LD', '/Brepro', '/DBC250_CONTROL_DLL', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$obj\bc250control.obj", "/Fe$Out\bc250control.dll",
    (Join-Path $repo 'tools\win\bc250kmd_cli\bc250kmd_cli.c'), '/link', '/Brepro', "/IMPLIB:$obj\bc250control.lib",
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|Creating library|bc250control.lib') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "bc250control.dll build failed ($LASTEXITCODE)" }
Remove-Item "$Out\bc250control.exp", "$Out\bc250control.lib" -ErrorAction SilentlyContinue

# 2b. bc250kmd_cli.exe from the same translation unit and the same flags, without BC250_CONTROL_DLL: the release ships
# the CLI and the DLL of one build, so the CLI has every command the DLL's source has (tester.10 shipped a CLI of the
# KMD branch without health, clock, telemetry, vram and sdmaib). Without arguments it prints its usage and exits 2.
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/Brepro', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$obj\bc250kmd_cli.obj", "/Fe$Out\bc250kmd_cli.exe",
    (Join-Path $repo 'tools\win\bc250kmd_cli\bc250kmd_cli.c'), '/link', '/Brepro',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "bc250kmd_cli.exe build failed ($LASTEXITCODE)" }
$usage = & "$Out\bc250kmd_cli.exe" 2>&1 | ForEach-Object { [string]$_ }
if ($LASTEXITCODE -ne 2 -or -not ($usage -match '^usage: bc250kmd_cli ')) { throw "bc250kmd_cli.exe without arguments: exit $LASTEXITCODE, no usage" }
Write-Host "  bc250kmd_cli.exe usage: $(@($usage).Count) lines"

# 3. The application.
& $csc /nologo /noconfig /nostdlib+ @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 /deterministic+ `
    "/win32manifest:$here\app.manifest" "/out:$Out\amdgpu_wddm_control.exe" (Get-ChildItem "$here\src\*.cs").FullName
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }

# 4. Smoke run: no window, exits by itself. The DWM observations of the gates go to obj\state, not to this PC's profile.
$env:AMDGPU_WDDM_CONTROL_STATE = Join-Path $obj 'state'
Remove-Item -Recurse -Force $env:AMDGPU_WDDM_CONTROL_STATE -ErrorAction SilentlyContinue
if (-not $NoSmoke) {
    $smoke = Join-Path $obj 'smoke.txt'
    Remove-Item $smoke -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke', "`"$smoke`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'smoke run did not exit within 60 s' }
    $text = if (Test-Path $smoke) { Get-Content $smoke -Raw } else { '' }
    if ($p.ExitCode -ne 0 -or $text -notmatch '(?m)^Status: ') { throw "smoke run failed (exit $($p.ExitCode)): $text" }
    ($text -split "`r?`n" | Select-Object -First 4) | ForEach-Object { Write-Host "  smoke: $_" }

    # The report path: a zip with the fixed entries and without this PC's user or computer name.
    $zip = Join-Path $obj 'report-smoke.zip'
    Remove-Item $zip, "$zip.error.txt" -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke-report', "`"$zip`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'report smoke run did not exit within 60 s' }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $zip)) { throw "report smoke run failed (exit $($p.ExitCode))" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $names = @($archive.Entries | ForEach-Object Name)
        foreach ($want in 'driver-state.txt', 'driver-log.txt', 'installed-files.txt', 'settings.txt', 'system.txt') {
            if ($names -notcontains $want) { throw "report smoke: $want missing" }
        }
        foreach ($entry in $archive.Entries) {
            $reader = New-Object IO.StreamReader($entry.Open())
            $body = $reader.ReadToEnd(); $reader.Dispose()
            foreach ($secret in $env:USERNAME, $env:COMPUTERNAME) {
                if ($secret.Length -ge 2 -and $body -match "(?<![A-Za-z0-9])$([regex]::Escape($secret))(?![A-Za-z0-9])") {
                    throw "report smoke: $($entry.Name) names this PC's user or computer"
                }
            }
        }
        Write-Host "  report smoke: $($names.Count) files, no user or computer name"
    } finally { $archive.Dispose() }

    # The Recovery dry runs: the plans of the exe itself, from the recorded BD-059 state and from this PC.
    function Invoke-DryRun([string[]]$ArgList, [string]$Name) {
        $file = Join-Path $obj "dry-$Name.txt"
        Remove-Item $file -ErrorAction SilentlyContinue
        $quoted = $ArgList | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }
        $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList (@($quoted) + @('--out', "`"$file`"")) -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "dry run $Name did not exit within 60 s" }
        [pscustomobject]@{ Code = $p.ExitCode; Text = $(if (Test-Path $file) { Get-Content $file -Raw } else { '' }) }
    }
    $snapshot = Join-Path $here 'test\snapshot-bd059.json'
    $params = 'HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
    $expect = [ordered]@{
        'reopen-gpu-path' = @(0, "set $params EnableGpuPresentBlit = 1 (DWord)", "delete $params InteropClosedReason")
        'desktop-gpu'     = @(3, 'refused: The GPU desktop path is closed for this start')
        'desktop-cpu'     = @(3, 'refused: The desktop already runs on the CPU route')
        'confirm-start'   = @(0, 'confirm this driver start', 'undo: no')
        'enable-dpm'      = @(0, "set $params DpmMode = 1 (DWord)", "set $params DpmMaxMHz = 1700 (DWord)")
        'reset-defaults'  = @(0, 'set HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter DwmForceCpu = 1 (DWord)')
        'undo'            = @(3, 'refused: There is no action to undo')
        'set-clocks'      = @(0, "delete $params DpmMaxMHz")
        'restart-compositor' = @(3, 'refused: restart-compositor is an operator escape', '--accept-bd060')
    }
    foreach ($e in $expect.GetEnumerator()) {
        $extra = @(switch ($e.Key) { 'enable-dpm' { '--ceiling', '1700' } 'set-clocks' { '--mode', 'unset', '--ceiling', 'unset' } })
        $r = Invoke-DryRun (@('--action', $e.Key) + $extra + @('--dry-run', '--snapshot', $snapshot)) $e.Key
        if ($r.Code -ne $e.Value[0]) { throw "dry run $($e.Key): exit $($r.Code), $($e.Value[0]) expected: $($r.Text)" }
        foreach ($want in $e.Value | Select-Object -Skip 1) {
            if (-not $r.Text.Contains($want)) { throw "dry run $($e.Key): '$want' missing: $($r.Text)" }
        }
        if (-not $r.Text.Contains('dry run: nothing was written')) { throw "dry run $($e.Key): no closing line" }
    }
    $r = Invoke-DryRun @('--action', 'reopen-gpu-path', '--snapshot', $snapshot) 'snapshot-without-dry-run'
    if ($r.Code -ne 2) { throw "a recorded snapshot without --dry-run must be refused (exit $($r.Code))" }
    $r = Invoke-DryRun @('--action', 'reopen-gpu-path', '--dry-run') 'this-pc'
    if (($r.Code -ne 0 -and $r.Code -ne 3) -or -not $r.Text.Contains('dry run: nothing was written')) { throw "dry run on this PC failed (exit $($r.Code)): $($r.Text)" }
    $u = Invoke-DryRun @('--action', 'set-clocks', '--ceiling', '1500', '--dry-run', '--snapshot', $snapshot) 'set-clocks-without-mode'
    if ($u.Code -ne 2) { throw "set-clocks without --mode must be refused as usage (exit $($u.Code))" }
    foreach ($a in 'desktop-gpu', 'desktop-cpu', 'undo') {
        $u = Invoke-DryRun @('--action', $a, '--dry-run', '--snapshot', $snapshot) "no-dwm-$a"
        if ($u.Text -match 'restart DWM|DWM restart|watchdog') { throw "dry run $a still names a DWM restart: $($u.Text)" }
    }
    $u = Invoke-DryRun @('--action', 'restart-compositor', '--accept-bd060', '--dry-run', '--snapshot', $snapshot) 'escape-accepted'
    if ($u.Code -ne 0 -or -not $u.Text.Contains('stop DWM in the active session') -or -not $u.Text.Contains('BD-060')) { throw "dry run of the accepted escape (exit $($u.Code)): $($u.Text)" }
    # --status records the session's DWM (the smoke runs above already did, into obj\state); a later reading keeps the
    # first observation, and one instance is unknown history, never "observed" without a replacement.
    $since = @()
    foreach ($n in 1, 2) {
        $st = Invoke-DryRun @('--status') "status-$n"
        if ($st.Code -ne 0 -or $st.Text -notmatch '(?m)^dwm-restart: (observed|unknown-history|unknown) \(boot') { throw "--status failed (exit $($st.Code)): $($st.Text)" }
        $statusLine = (($st.Text -split "`r?`n") | Where-Object { $_ -like 'dwm-restart:*' } | Select-Object -First 1)
        $since += if ($statusLine -match 'watched since (\S+ by \w+)\)') { $Matches[1] } else { '-' }
    }
    if ($statusLine -notlike 'dwm-restart: unknown *') {
        $rec = Join-Path $env:AMDGPU_WDDM_CONTROL_STATE 'dwm-observations.json'
        if (-not (Test-Path $rec)) { throw "--status did not record the session's DWM in $rec" }
        if ($since[0] -eq '-' -or $since[0] -ne $since[1]) { throw "--status did not keep the first observation: $($since -join ' / ')" }
        # The installer's record, written as tools\release\installer\dwm-session.ps1 writes it (boot time from
        # Win32_OperatingSystem, logon time from WTSSessionInfo, the DWM from Win32_Process), must be read as this
        # session's earliest observation: the app's own epoch readings agree with the installer's.
        if (-not ('BuildGate.Wts' -as [type])) {
            Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
namespace BuildGate { public static class Wts {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct I { public int a, b, c, d, e, f, g, h;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string w; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 17)] public string x;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 21)] public string y; public long t0, t1, t2, LogonTime, t4; }
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode)] static extern bool WTSQuerySessionInformationW(IntPtr s, int id, int c, out IntPtr b, out int n);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr m);
    public static long LogonTime(int id) { IntPtr b; int n; if (!WTSQuerySessionInformationW(IntPtr.Zero, id, 24, out b, out n)) return -1;
        try { return ((I)Marshal.PtrToStructure(b, typeof(I))).LogonTime; } finally { WTSFreeMemory(b); } } } }
'@
        }
        $session = (Get-Process -Id $PID).SessionId
        $logon = [BuildGate.Wts]::LogonTime($session)
        $dwm = @(Get-CimInstance Win32_Process -Filter "Name='dwm.exe'" | Where-Object { $_.SessionId -eq $session } | Sort-Object CreationDate | Select-Object -Last 1)
        if ($session -ne 0 -and $logon -gt 0 -and $dwm.Count -eq 1) {
            $created = $dwm[0].CreationDate.ToUniversalTime()
            $rec = [ordered]@{ boot_utc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'); session = $session
                logon_utc = [DateTime]::FromFileTimeUtc($logon).ToString('o'); dwm_pid = [int]$dwm[0].ProcessId; dwm_created_utc = $created.ToString('o')
                recorded_utc = $created.AddSeconds(1).ToString('o'); recorded_by = 'start-confirm' }
            $baseline = Join-Path $env:AMDGPU_WDDM_CONTROL_STATE 'dwm-baseline.json'
            [IO.File]::WriteAllText($baseline, ([ordered]@{ schema = 1; records = @($rec) } | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
            $st = Invoke-DryRun @('--status') 'status-installer-record'
            $line = (($st.Text -split "`r?`n") | Where-Object { $_ -like 'dwm-restart:*' } | Select-Object -First 1)
            if ($line -notlike 'dwm-restart: unknown-history *' -or $line -notmatch 'instances seen 1, watched since \S+ by start-confirm\)') { throw "--status did not read the installer's record of this session: $line" }
            Remove-Item $baseline
            Write-Host "  installer record: read as this session's first observation"
        }
    }
    # A real run writes --out too. Only where it cannot change anything: without administrator it stops at once.
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        $u = Invoke-DryRun @('--action', 'reopen-gpu-path') 'real-run-not-elevated'
        if ($u.Code -ne 5 -or -not $u.Text.Contains('needs administrator') -or -not $u.Text.Contains('exit 5')) { throw "a real run must write --out (exit $($u.Code)): $($u.Text)" }
    }
    Write-Host "  status of this PC: $statusLine"
    Write-Host "  dry runs: $($expect.Count) actions planned from the BD-059 snapshot as expected; this PC: $((($r.Text -split "`r?`n") | Where-Object { $_ -match '^\s+(refused|change):' } | Select-Object -First 1).Trim())"
}

if (-not $NoSmoke) {
    foreach ($scale in '1', '1.25', '1.5') {
        $dir = Join-Path $obj "render-$scale"
        $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke-render', "`"$dir`"", $scale -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "render at $scale did not exit within 60 s" }
        $layout = Get-Content (Join-Path $dir 'layout.txt') -Raw -ErrorAction SilentlyContinue
        if ($p.ExitCode -ne 0) { throw "render at scale $scale (exit $($p.ExitCode)): $layout" }
    }
    Write-Host "  render: 5 pages at 96, 120 and 144 DPI, no overlapping controls ($obj\render-*)"
}

Get-ChildItem $Out -File | ForEach-Object {
    '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 8), $_.Name
}
