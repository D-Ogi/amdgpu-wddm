$ErrorActionPreference = 'Stop'
. P:\bc-250\scratch\bd081\wt\tools\build\common.ps1
$out = 'P:\bc-250\scratch\bd081\spy\out'
$wdk = 'P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0'
$saved = Save-ProcessEnvironment
try {
    Import-VsDevEnvironment -TempDir $out -Arch x64 | Out-Null
    $env:INCLUDE = "$wdk\um;$wdk\shared;$env:INCLUDE"
    & cl.exe /nologo /W3 /EHsc "/Fo$out\entryspy.obj" P:\bc-250\scratch\bd081\spy\entryspy.cpp "/Fe:$out\entryspy.exe" | Out-Null
    if ($LASTEXITCODE) { throw 'build' }
} finally { Restore-ProcessEnvironment $saved }
'=== pass 1: the UMD exports both entries'
& "$out\entryspy.exe" show all
'=== pass 2: OpenAdapter10_2 hidden, D3D10.0 runtime only'
& "$out\entryspy.exe" hide102 d3d10
