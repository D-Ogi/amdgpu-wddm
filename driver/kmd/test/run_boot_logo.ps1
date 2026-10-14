param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\boot-logo",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path -Parent (Split-Path -Parent $kmd)
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'

$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
$env:INCLUDE = ''
$env:LIB = ''

# The picture is generated source, checked in. The generator is not run here: the build must not need Pillow,
# and a changed asset has to be committed, not produced on the way past.
$hashes = @{}
foreach ($path in 'driver/kmd/boot_logo.c', 'driver/kmd/boot_logo.h', 'driver/kmd/boot_logo_asset.inc',
                  'driver/kmd/test/boot_logo_test.c') {
    $hashes[$path] = (Get-FileHash "$repo/$path").Hash
    Write-Host ("  {0}  {1}" -f $hashes[$path].Substring(0, 16), $path)
}

$incUser = @("/I$kmd", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile and link (host, no WDK header)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/TC', '/W4', '/WX', '/Od', '/Zi') + $incUser +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\boot_logo_test.exe") +
    @((Join-Path $kmd 'boot_logo.c'), (Join-Path $here 'boot_logo_test.c')) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
& "$Out\boot_logo_test.exe"
$code = $LASTEXITCODE

Write-Host 'compile-check (kernel flags, same source, no link)'
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$kmd", "/Fo$obj\kernel-", "/Fd$obj\clkernel.pdb")
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + @((Join-Path $kmd 'boot_logo.c')))

# The three lines this feature writes to the driver log live in display.c, which no host test compiles: it needs
# the WDK and the whole device. Their FORM is pinned here instead, because a support file is read by a person and
# by the trial analyser (scratch\boot-nagi\lab\analyse-start.py keys on "boot logo:"), so a renamed field is a
# broken tool. The widths are the guardlog-width gate's business, not this one's.
Write-Host 'log line forms (display.c)'
$display = Get-Content (Join-Path $kmd 'display.c') -Raw
$forms = @(
    @{ what = 'the draw';           text = '"boot logo: %s on %ux%u anchor %u, %u pixels, %u bytes, %u us"' },
    @{ what = 'the handoff refusal'; text = '"boot logo: skipped, target %u is a driver handoff, not a boot"' },
    @{ what = 'off by setting';     text = '"boot logo: off by setting (BootLogo 0)"' },
    @{ what = 'the scanout';        text = '"boot logo: scanout 0x%llX inherited 0x%llX otg master %u blank %u control 0x%08X"' },
    @{ what = 'a failed read';      text = '"boot logo: scanout not read at %s, status 0x%08X"' }
)
$bad = 0
foreach ($form in $forms) {
    if ($display.Contains($form.text)) { Write-Host ("  ok   {0}" -f $form.what) }
    else { Write-Host ("  FAIL {0}: display.c does not hold {1}" -f $form.what, $form.text); $bad++ }
}
# A failed register read must not print a value: zeros would read as "scanning out nothing from a blanked pipe",
# which is a conclusion about hardware that was never read.
if ($display -match 'scanout not read at %s, status 0x%08X"\s*,\s*which\s*,\s*\(ULONG\)status\s*\)') {
    Write-Host '  ok   a failed read prints its own marker and no register value'
} else {
    Write-Host '  FAIL the failed-read line does not print exactly the step and the status'; $bad++
}
if ($bad) { Write-Host "$bad log line form(s) wrong"; if ($code -eq 0) { $code = 1 } }

Write-Host ''
Write-Host "boot_logo_test.exe exit code $code"
exit $code
