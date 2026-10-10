# Production recovery functions with fake kernel APIs; no host registry or GPU access.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [Parameter(Mandatory=$true)][string]$Kits,
    [string]$KitVersion = '10.0.26100.0',
    [switch]$NoPendingMarker, [switch]$ClearBeforeData, [switch]$AdminReadOnly
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = $Out
$env:TMP = $Out
$env:INCLUDE = ''
$env:LIB = ''
& python -B "$PSScriptRoot\wave_recovery_extract.py" --root $repo --out "$Out\wave_recovery_actual.c"
if ($LASTEXITCODE -ne 0) { throw 'Recovery source extraction failed' }
$source = [IO.File]::ReadAllText("$Out\wave_recovery_actual.c")
# Negative controls change only the extracted copy, never the production source.
if ($NoPendingMarker) {
    $source = $source.Replace('status = WriteDword(key, L"WavesPending", WavesPending ? 1u : 0u);',
                              'status = WriteDword(key, L"WavesPending", WavesPending ? 0u : 0u);')
}
if ($ClearBeforeData) {
    $source = $source.Replace('if (NT_SUCCESS(status)) status = WriteDword(recovery, L"WavesPending", 0);', '')
    $source = $source.Replace('if (NT_SUCCESS(status)) status = ZwSetValueKey(key, &name, 0, REG_BINARY,',
                              'if (NT_SUCCESS(status)) status = WriteDword(recovery, L"WavesPending", 0); if (NT_SUCCESS(status)) status = ZwSetValueKey(key, &name, 0, REG_BINARY,')
}
if ($AdminReadOnly) { $source = $source.Replace('ACL_REVISION, KEY_READ | DELETE, (PSID)adminSid', 'ACL_REVISION, KEY_READ, (PSID)adminSid') }
[IO.File]::WriteAllText("$Out\wave_recovery_actual.c", $source, (New-Object Text.UTF8Encoding($false)))
& $cl /nologo /TC /std:c11 /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\$KitVersion\ucrt" "/Fo$Out\" "/Fe$Out\wave_recovery_test.exe" "$Out\wave_recovery_actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'Wave recovery host build failed' }
& "$Out\wave_recovery_test.exe"
exit $LASTEXITCODE
