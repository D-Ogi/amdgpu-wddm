# Isolated Windows adapter/client fixture. No service or device is accessed.
param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits,
      [ValidateSet('x64','x86')][string]$Arch='x64',[switch]$BuildOnly)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force "$Out\tmp" | Out-Null
$env:TEMP="$Out\tmp";$env:TMP=$env:TEMP
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
$env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB="$($msvc.FullName)\lib\$Arch;$libs\ucrt\$Arch;$libs\um\$Arch"
& "$($msvc.FullName)\bin\Hostx64\$Arch\cl.exe" /nologo /TC /W4 /WX /O2 /MT /DAMDGPU_STATE_ADAPTER_TEST `
    "/Fo$Out\" "/Fe$Out\state_windows_test.exe" "$repo\driver\state\state.c" "$repo\driver\state\transfer.c" `
    "$repo\driver\state\windows_lock.c" "$repo\driver\state\windows.c" "$repo\driver\state\client.c" `
    "$repo\driver\state\windows_test.c" /link advapi32.lib ole32.lib onecore.lib
if($LASTEXITCODE){throw 'Client concurrency fixture compile failed'}
if(!$BuildOnly) {
    & "$Out\state_windows_test.exe" "$repo\driver\state\amdgpu-state.bin"
    if($LASTEXITCODE){throw 'Client concurrency fixture failed'}
}
