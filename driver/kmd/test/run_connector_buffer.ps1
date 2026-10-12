param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [string]$Kits = ''
)
$ErrorActionPreference = 'Stop'
if (!$Kits) { $Kits = Join-Path $Root 'toolchain/nuget' }
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp/c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64/c'
$version = '10.0.26100.0'
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cl = "$($msvc.FullName)/bin/Hostx64/x64/cl.exe"
$oldPath, $oldInclude, $oldLib = $env:PATH, $env:INCLUDE, $env:LIB
try {
    $env:PATH = "$(Split-Path $cl);$oldPath"
    $env:INCLUDE = ''; $env:LIB = ''
    $obj = Join-Path $Out 'connector-buffer-obj'
    New-Item -ItemType Directory -Force $obj | Out-Null
    $body = Get-Content "$repo/driver/kmd/connector_buffer.c" -Raw
    if ([regex]::Matches($body,'#include "bc250kmd\.h"').Count -ne 1) {
        throw 'Expected exactly one production-header include'
    }
    $body = $body.Replace('#include "bc250kmd.h"','')
    [IO.File]::WriteAllText((Join-Path $Out 'connector_buffer_actual.inc'),$body)
    $argsCl = @('/nologo','/TC','/W4','/WX','/O2','/MT',
        "/I$($msvc.FullName)/include", "/I$sdk/Include/$version/ucrt", "/Fo$obj\", "/Fe$Out/connector_buffer_test.exe",
        "/I$Out", "$PSScriptRoot/connector_buffer_test.c", '/link',
        "/LIBPATH:$($msvc.FullName)/lib/x64", "/LIBPATH:$sdklib/ucrt/x64", "/LIBPATH:$sdklib/um/x64")
    & $cl @argsCl
    if ($LASTEXITCODE -ne 0) { throw 'Connector buffer host build failed' }
    & "$Out/connector_buffer_test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Connector buffer parameter tests failed' }
} finally {
    $env:PATH=$oldPath; $env:INCLUDE=$oldInclude; $env:LIB=$oldLib
}
