# Compiler regression gate: no driver load, device creation, or GPU work.
param([Parameter(Mandatory)][string]$OutputDir, [string]$VsInstall)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/common.ps1"
$saved = Save-ProcessEnvironment
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$null = New-Item -ItemType Directory -Force $OutputDir
$checks = 0
try {
    $env:TEMP = $OutputDir; $env:TMP = $OutputDir
    # Controls start with no caller-supplied warning policy.
    $env:CL = $null; $env:_CL_ = $null
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch x86
    $policy = $env:_CL_
    if ($policy -notmatch '/we4229' -or $policy -notmatch '/we4440') { throw 'x86 compiler policy missing' }
    Push-Location $OutputDir
    try {
        $null = New-Item -ItemType Directory -Force external
        $bad = 'typedef int (* __stdcall callback)(int);'
        $redef = 'typedef int __cdecl function_type(int); typedef function_type __stdcall ignored_type;'
        $good = 'typedef int (__stdcall *callback)(int); int __stdcall target(int a) { return a; } callback p = target;'
        Set-Content -Encoding ascii external/bad.h $bad
        Set-Content -Encoding ascii external/good.h $good
        $cases = @(
            @{ Name='pointer-c'; Ext='c'; Text=$bad; Diagnostic='C4229'; Extra=@() },
            @{ Name='pointer-cpp'; Ext='cpp'; Text=$bad; Diagnostic='C4229'; Extra=@() },
            @{ Name='redefinition'; Ext='cpp'; Text=$redef; Diagnostic='C4440'; Extra=@() },
            @{ Name='dependency'; Ext='c'; Text='#include <bad.h>'; Diagnostic='C4229'; Extra=@('/external:W0', "/external:I$OutputDir/external") },
            @{ Name='valid-c'; Ext='c'; Text=$good; Diagnostic=$null; Extra=@() },
            @{ Name='valid-cpp'; Ext='cpp'; Text=$good; Diagnostic=$null; Extra=@() },
            @{ Name='valid-dependency'; Ext='cpp'; Text='#include <good.h>'; Diagnostic=$null; Extra=@('/external:W0', "/external:I$OutputDir/external") }
        )
        foreach ($case in $cases) {
            $src = "$($case.Name).$($case.Ext)"
            Set-Content -Encoding ascii $src $case.Text
            foreach ($enabled in $false, $true) {
                $env:_CL_ = if ($enabled) { $policy } else { $null }
                $log = "$($case.Name)-$enabled.log"
                & cl.exe /nologo /c /W0 /WX- @($case.Extra) $src *> $log
                $code = $LASTEXITCODE
                $diagnostic = Get-Content -Raw $log
                if ($enabled -and $case.Diagnostic) {
                    if ($code -eq 0 -or $diagnostic -notmatch $case.Diagnostic) { throw "Expected $($case.Diagnostic) refusal: $log" }
                } elseif ($code -ne 0) { throw "Positive/control compile failed: $log" }
                $checks++
            }
        }
        # The recipe record and nested Meson dependencies get the same flags.
        foreach ($arch in 'x86', 'x64') {
            $opts = @(Add-ReproducibleMesonOptions @('-Dc_args=/O2', '-Dcpp_args=/O1') 'source' 'build' -Arch $arch)
            foreach ($kind in 'c_args', 'cpp_args') {
                $option = @($opts | Where-Object { $_.StartsWith("-D$kind=") })
                if ($option.Count -ne 1) { throw "Duplicate $kind" }
                $expected = $arch -eq 'x86'
                foreach ($warning in '4229','4440') {
                    if (($option[0].Contains("/we$warning")) -ne $expected) { throw "Meson $arch $kind warning policy" }
                    $checks++
                }
            }
        }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }
Write-Output "$checks calling-convention checks passed (x86 compiler; no GPU work)"
exit 0
