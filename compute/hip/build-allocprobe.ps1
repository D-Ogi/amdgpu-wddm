# Host-only build and parser gate. Does not open the adapter or run a lab test.
param(
    [string]$Root = $(
        if ($env:BC250_ROOT) { $env:BC250_ROOT } else {
            $probe = $PSScriptRoot
            while ($probe -and -not (Test-Path (Join-Path $probe 'toolchain\nuget'))) {
                $probe = Split-Path -Parent $probe
            }
            if ($probe) { $probe } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
        }
    ),
    [string]$Kits = '',
    [string]$Out = '',
    [Parameter(Mandatory = $true)][string]$Library,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$LibrarySha256
)
$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\allocprobe' }
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if ((Get-FileHash -LiteralPath $Library -Algorithm SHA256).Hash -ne $LibrarySha256) {
    throw 'Layer 1 library does not match the requested diagnostic baseline'
}
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$version = '10.0.26100.0'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio compiler not found' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$oldTemp = $env:TEMP; $oldTmp = $env:TMP
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$source = Join-Path $PSScriptRoot 'tools\allocprobe.c'
$header = Join-Path $PSScriptRoot 'include\bc250hsa.h'
$exe = Join-Path $Out 'allocprobe.exe'
try {
    $arguments = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/std:c11', '/Brepro',
        "/I$($msvc.FullName)\include", "/I$sdk\Include\$version\ucrt",
        "/I$sdk\Include\$version\um", "/I$sdk\Include\$version\shared",
        "/I$PSScriptRoot\include", $source, $Library,
        "/Fo$Out\allocprobe.obj", "/Fe$exe", '/link', '/Brepro',
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/LIBPATH:$sdkLib\ucrt\x64",
        "/LIBPATH:$sdkLib\um\x64", 'gdi32.lib')
    & $cl @arguments
    if ($LASTEXITCODE -ne 0) { throw 'allocprobe build failed' }
    & $exe --selftest
    if ($LASTEXITCODE -ne 0) { throw 'allocprobe parser selftest failed' }
    $pins = foreach ($path in @($source, $header, $Library, $exe, $PSCommandPath)) {
        [pscustomobject]@{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
    $pins | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Out 'pins.json') -Encoding utf8
    $pins | Format-Table -AutoSize
} finally {
    $env:TEMP = $oldTemp; $env:TMP = $oldTmp
}
