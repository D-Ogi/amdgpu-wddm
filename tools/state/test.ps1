# CPU-only contract tests. -Registry adds an isolated, volatile HKCU fixture.
param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits,
      [ValidateSet('x64','x86')][string]$Arch='x64',[switch]$Registry)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
& python -B "$PSScriptRoot\generate.py" --check
if ($LASTEXITCODE) { throw 'State generation check failed' }
New-Item -ItemType Directory -Force "$Out\tmp" | Out-Null
$env:TEMP="$Out\tmp"; $env:TMP=$env:TEMP
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
$env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt"
$env:LIB="$($msvc.FullName)\lib\$Arch;$libs\ucrt\$Arch;$libs\um\$Arch"
& "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/Fo$Out\" "/Fe$Out\state_test.exe" "$repo\driver\state\state.c" "$repo\driver\state\state_test.c"
if ($LASTEXITCODE) { throw 'State contract compile failed' }
& "$Out\state_test.exe" "$repo\driver\state\amdgpu-state.bin"
if ($LASTEXITCODE) { throw 'State contract tests failed' }
& "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/Fo$Out\" "/Fe$Out\transfer_test.exe" `
    "$repo\driver\state\state.c" "$repo\driver\state\transfer.c" "$repo\driver\state\transfer_test.c"
if ($LASTEXITCODE) { throw 'Transfer compile failed' }
& "$Out\transfer_test.exe"
if ($LASTEXITCODE) { throw 'Transfer tests failed' }
$env:INCLUDE += ";$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
& "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT /c "/Fo$Out\state_windows.obj" "$repo\driver\state\windows.c"
if ($LASTEXITCODE) { throw 'State Windows adapter compile failed' }
& "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT /c "/Fo$Out\windows_migration.obj" "$repo\driver\state\windows_migration.c"
if ($LASTEXITCODE) { throw 'Legacy migration adapter compile failed' }
& "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT /c "/Fo$Out\windows_lock.obj" "$repo\driver\state\windows_lock.c"
if ($LASTEXITCODE) { throw 'Writer lock compile failed' }
foreach($source in 'identity','windows_handoff') {
    & "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT /c "/Fo$Out\$source.obj" "$repo\driver\state\$source.c"
    if ($LASTEXITCODE) { throw "$source compile failed" }
}
$production=[IO.File]::ReadAllText("$repo\driver\kmd\state_transaction.c")
foreach ($include in '#include <ntifs.h>','#include <ntstrsafe.h>','#include "state_transaction.h"','#include "../state/transfer.h"') {
    $production=$production.Replace($include,'')
}
$template=[IO.File]::ReadAllText("$repo\driver\state\transaction_test.c")
[IO.File]::WriteAllText("$Out\transaction_actual.c",$template.Replace('/* ACTUAL_TRANSACTION */',$production))
& "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\state" "/Fo$Out\" `
    "/Fe$Out\transaction_test.exe" "$repo\driver\state\state.c" "$repo\driver\state\transfer.c" "$Out\transaction_actual.c" /link advapi32.lib
if ($LASTEXITCODE) { throw 'Kernel transaction mock compile failed' }
& "$Out\transaction_test.exe"
if ($LASTEXITCODE) { throw 'Kernel transaction tests failed' }
& "$PSScriptRoot\test-service-root.ps1" -Out "$Out\service-root" -Kits $Kits -Arch $Arch
& "$PSScriptRoot\test-lifecycle.ps1" -Out "$Out\lifecycle" -Kits $Kits -Arch $Arch
& "$PSScriptRoot\test-device.ps1" -Out "$Out\device" -Kits $Kits -Arch $Arch
# Mutation controls: each run patches one production statement and must fail with a named assertion.
# Their FAIL lines are expected and labelled; a control that passes, or a suite above that fails, throws.
if ($Arch -eq 'x64') {
    & "$PSScriptRoot\test-lifecycle.ps1" -Out "$Out\lifecycle-flush-control" -Kits $Kits -Arch $Arch -IgnoreFirstFlushError
    & "$PSScriptRoot\test-device.ps1" -Out "$Out\device-validation-control" -Kits $Kits -Arch $Arch -Mutation activate-during-validation
    & "$PSScriptRoot\test-device.ps1" -Out "$Out\device-start-control" -Kits $Kits -Arch $Arch -Mutation skip-start-activation
}
if ($Registry) {
    & "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT /DAMDGPU_STATE_ADAPTER_TEST `
        "/Fo$Out\" "/Fe$Out\state_windows_test.exe" "$repo\driver\state\state.c" `
        "$repo\driver\state\transfer.c" "$repo\driver\state\windows_lock.c" `
        "$repo\driver\state\windows.c" "$repo\driver\state\client.c" "$repo\driver\state\windows_test.c" /link advapi32.lib ole32.lib onecore.lib
    if ($LASTEXITCODE) { throw 'Windows state adapter test compile failed' }
    & "$Out\state_windows_test.exe" "$repo\driver\state\amdgpu-state.bin"
    if ($LASTEXITCODE) { throw 'Windows state adapter tests failed' }
}
