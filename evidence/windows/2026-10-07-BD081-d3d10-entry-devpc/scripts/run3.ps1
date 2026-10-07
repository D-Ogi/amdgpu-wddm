$ErrorActionPreference = 'Stop'
. P:\bc-250\scratch\bd081\wt\tools\build\common.ps1
$out = 'P:\bc-250\scratch\bd081\spy\out-x86'
New-Item -ItemType Directory -Force $out | Out-Null
$wdk = 'P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0'
$saved = Save-ProcessEnvironment
try {
    Import-VsDevEnvironment -TempDir $out -Arch x86 | Out-Null
    $env:INCLUDE = "$wdk\um;$wdk\shared;$env:INCLUDE"
    $env:LIB = "P:\bc-250\toolchain\nuget\microsoft.windows.sdk.cpp.x86\c\um\x86;$env:LIB"
    & cl.exe /nologo /W3 /EHsc "/Fo$out\entryspy.obj" P:\bc-250\scratch\bd081\spy\entryspy.cpp "/Fe:$out\entryspy.exe"
    if ($LASTEXITCODE) { throw 'build' }
} finally { Restore-ProcessEnvironment $saved }
'=== x86: the UMD exports both entries'
& "$out\entryspy.exe" show all
