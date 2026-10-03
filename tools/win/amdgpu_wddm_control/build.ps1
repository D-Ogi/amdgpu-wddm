# Builds the amdgpu-wddm control application: amdgpu_wddm_control.exe (.NET Framework 4.8, part of Windows 10/11,
# nothing to install on a tester's PC) and bc250control.dll (tools/win/bc250kmd_cli/bc250kmd_cli.c with
# BC250_CONTROL_DLL). Compilers from the installed Visual Studio, headers and import libraries from the SDK NuGet
# packages under -Kits. Deterministic: the same sources give the same bytes (csc /deterministic, cl/link /Brepro).
#
#   pwsh tools\win\amdgpu_wddm_control\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\release\control-app\build\app
#
# Gates, in order, each stops the build: the unit tests (test/UnitTests.cs against driver/kmd/bc250kmd_escape.h and
# the D3D12 shell's switch names), the DLL and app compiles with warnings as errors, and a smoke run of the exe with
# --smoke (no window: the pages are built and refreshed once, their text written to smoke.txt). The smoke run passes
# on a PC without a BC-250 when it reports the driver as not found.

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
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
$pure = 'KmdReply.cs', 'Profiles.cs', 'Redactor.cs', 'ManifestCheck.cs' | ForEach-Object { Join-Path $here "src\$_" }

# 1. Unit tests of the pure parts.
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ /langversion:7.3 /deterministic+ `
    "/out:$obj\unit-tests.exe" @pure (Join-Path $here 'test\UnitTests.cs')
if ($LASTEXITCODE -ne 0) { throw "unit test compile failed ($LASTEXITCODE)" }
& "$obj\unit-tests.exe" $repo | ForEach-Object { Write-Host "  $_" }
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

# 3. The application.
& $csc /nologo /noconfig /nostdlib+ @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 /deterministic+ `
    "/win32manifest:$here\app.manifest" "/out:$Out\amdgpu_wddm_control.exe" (Get-ChildItem "$here\src\*.cs").FullName
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }

# 4. Smoke run: no window, exits by itself.
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
}

Get-ChildItem $Out -File | ForEach-Object {
    '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 8), $_.Name
}
