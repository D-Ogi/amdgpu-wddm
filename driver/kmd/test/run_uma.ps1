# CPU-only tests; callbacks operate on an in-memory block, never hardware.
param([Parameter(Mandatory)][string]$Root, [string]$Out = "$Root\scratch\uma-windows\host",
      [ValidateSet("none","checksum","stale","noop","unchanged","marker")][string]$Mutation = "none")
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl = "$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$policySource = "$repo\driver\shim\bc250_uma.c"
if ($Mutation -ne 'none') {
    $text = [IO.File]::ReadAllText($policySource)
    $old = switch ($Mutation) {
        'checksum' { 'return word(block + 4) == checksum(block);' }
        'unchanged' { 'if (current == block[offsets[i]]) continue;' }
        'marker' { 'if (!io->read(io->context, 0, &marker) || marker != 0) return 0;' }
        'stale' { 'if (memcmp(current, expected, sizeof(current))) return BC250_UMA_STALE;' }
        'noop' { 'if (word(current + 26) == target_mib) return BC250_UMA_NO_CHANGE;' }
    }
    if ($text.Split(@($old), [StringSplitOptions]::None).Count -ne 2) { throw 'Mutation source anchor changed' }
    $replacement = if ($Mutation -eq 'checksum') { 'return 1;' } elseif ($Mutation -eq 'marker') { '(void)marker;' } else { '' }
    $policySource = "$Out\uma_mutated.c"
    [IO.File]::WriteAllText($policySource, $text.Replace($old, $replacement))
}
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\uma_test.exe" $policySource "$PSScriptRoot\uma_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'UMA policy host build failed' }
$output = @(& "$Out\uma_test.exe")
$code = $LASTEXITCODE
$output | ForEach-Object { Write-Output $_ }
if ($Mutation -eq 'none') {
    if ($code -ne 0) { throw 'UMA policy tests failed' }
} elseif ($code -ne 1 -or -not ($output -match '^FAIL line')) {
    throw 'Compiled mutation did not fail a runtime CHECK'
}
[pscustomobject]@{mutation=$Mutation;exit=$code;summary=$output[-1]} | ConvertTo-Json | Set-Content "$Out\result.json"
Get-FileHash $policySource,"$PSCommandPath","$repo\driver\shim\include\bc250_uma.h","$PSScriptRoot\uma_test.c","$Out\uma_test.exe" | Select-Object Path,Hash | ConvertTo-Json | Set-Content "$Out\pins.json"

