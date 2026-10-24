# Compiler regression gate: no driver load, device creation, or GPU work.
param([Parameter(Mandatory)][string]$OutputDir, [string]$VsInstall,
      # The LLVM Windows toolchain of the clang-cl recipes (default <BC250_ROOT>\toolchain\llvm-23.1.4-win64).
      [string]$ClangToolchain)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/common.ps1"
if (-not $ClangToolchain) { $ClangToolchain = Join-Path (Get-Bc250Root (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) 'toolchain\llvm-23.1.4-win64' }
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
        # clang-cl for x86: a recipe that does not name the i686 target is refused, and one that does gets the
        # clang-cl policy in place of the MSVC one (common.ps1).
        $refusedX86 = $false
        try { $null = Add-ReproducibleMesonOptions @('-Dc_args=/O2') 'source' 'build' -Arch x86 -Compiler clang-cl } catch { $refusedX86 = $true }
        if (-not $refusedX86) { throw 'clang-cl x86 without -ClangTarget was not refused' }
        $checks++
        $clangPolicy = @('/clang:-Werror=ignored-attributes', '/clang:-Werror=cast-calling-convention')
        foreach ($arch in 'x86', 'x64') {
            $opts = @(Add-ReproducibleMesonOptions @('-Dc_args=/O2', '-Dcpp_args=/O1') 'source' 'build' -Arch $arch -Compiler clang-cl -ClangTarget 'i686-pc-windows-msvc')
            foreach ($kind in 'c_args', 'cpp_args') {
                $option = @($opts | Where-Object { $_.StartsWith("-D$kind=") })
                if ($option.Count -ne 1) { throw "Duplicate clang-cl $kind" }
                foreach ($switch in $clangPolicy) {
                    if (($option[0].Contains($switch)) -ne ($arch -eq 'x86')) { throw "Meson clang-cl $arch $kind policy $switch" }
                    $checks++
                }
                if ($option[0] -match '/we4229|/we4440') { throw "Meson clang-cl $arch $kind has the MSVC policy" }
                $checks++
            }
        }
        # The compile-command gate of build-vkd3d.ps1 and build-mesa.ps1 (Test-ClangX86Policy, common.ps1): a command
        # with the target and the policy passes; a command without the target or a switch, or with a switch that
        # turns the policy off or makes a directory a system directory, is refused, except a system directory that
        # the caller admits (the 'dep' directory of these cases); an empty database stops the build.
        $switches = '/clang:-Werror=ignored-attributes /clang:-Werror=cast-calling-convention'
        $full = "clang-cl.exe --target=i686-pc-windows-msvc /c $switches"
        $gateCases = @(
            @{ Name='policy'; Command="$full /Isrc a.c"; Refused=$false },
            @{ Name='quoted'; Command="`"C:\llvm\clang-cl.exe`" --target=i686-pc-windows-msvc /clang:-Werror=ignored-attributes `"/clang:-Werror=cast-calling-convention`" a.c"; Refused=$false },
            @{ Name='no-target'; Command="clang-cl.exe /c $switches a.c"; Refused=$true },
            @{ Name='no-cast-switch'; Command='clang-cl.exe --target=i686-pc-windows-msvc /clang:-Werror=ignored-attributes a.c'; Refused=$true },
            @{ Name='admitted-isystem'; Command="$full /clang:-isystemdep a.c"; Refused=$false; Admitted=1 },
            @{ Name='admitted-external'; Command="$full /external:Idep\ a.c"; Refused=$false; Admitted=1 },
            @{ Name='other-isystem'; Command="$full /clang:-isystemother a.c"; Refused=$true },
            @{ Name='other-external'; Command="$full /external:Iother a.c"; Refused=$true },
            @{ Name='imsvc'; Command="$full /imsvcother a.c"; Refused=$true },
            @{ Name='split-isystem'; Command="$full -isystem dep a.c"; Refused=$true },
            @{ Name='external-w0'; Command="$full /external:W0 a.c"; Refused=$true },
            @{ Name='system-prefix'; Command="$full /clang:--system-header-prefix=dep a.c"; Refused=$true },
            @{ Name='W0'; Command="$full /W0 a.c"; Refused=$true },
            @{ Name='w'; Command="$full -w a.c"; Refused=$true },
            @{ Name='wno-cast'; Command="$full /clang:-Wno-cast-calling-convention a.c"; Refused=$true },
            @{ Name='wno-error-attr'; Command="$full -Wno-error=ignored-attributes a.c"; Refused=$true }
        )
        foreach ($case in $gateCases) {
            $dir = Join-Path $OutputDir "policy-gate-$($case.Name)"
            $null = New-Item -ItemType Directory -Force $dir
            ConvertTo-Json -InputObject @([ordered]@{ directory = $dir; command = $case.Command; file = 'a.c' }) |
                Set-Content -Encoding utf8 (Join-Path $dir 'compile_commands.json')
            $gate = Test-ClangX86Policy $dir 'i686-pc-windows-msvc' @((Join-Path $dir 'dep'))
            if ($gate.compile_commands -ne 1) { throw "policy gate $($case.Name): $($gate.compile_commands) commands read" }
            if (($gate.refused.Count -gt 0) -ne $case.Refused) { throw "policy gate $($case.Name): refused [$($gate.refused -join '; ')]" }
            $admitted = @($gate.system_includes.Values)[0]
            $want = if ($case.ContainsKey('Admitted')) { $case.Admitted } else { 0 }
            if ($admitted -ne $want) { throw "policy gate $($case.Name): the admitted directory counts $admitted commands, not $want" }
            $stopped = $false
            try { Assert-ClangX86Policy $gate $dir 6>$null } catch { $stopped = $true }
            if ($stopped -ne $case.Refused) { throw "policy gate $($case.Name): Assert-ClangX86Policy stopped=$stopped" }
            $checks++
        }
        $empty = Join-Path $OutputDir 'policy-gate-empty'
        $null = New-Item -ItemType Directory -Force $empty
        Set-Content -Encoding utf8 (Join-Path $empty 'compile_commands.json') '[]'
        $stopped = $false
        try { Assert-ClangX86Policy (Test-ClangX86Policy $empty 'i686-pc-windows-msvc') $empty 6>$null } catch { $stopped = $true }
        if (-not $stopped) { throw 'policy gate: an empty compile_commands.json did not stop the build' }
        $checks++
        # The same cases through the i686 clang-cl of the recipes. clang stops on its own where a calling convention
        # changes on a redeclaration; the two warnings of the policy stop a convention that clang ignores (on data)
        # and a cast of a function to a pointer of another convention. A convention after the * of a function
        # pointer is not a case for clang: it applies to the function type (MSVC ignores it, C4229, and gives
        # cdecl), which the code check below shows. A header in a system directory gets no warning, so the
        # recipe refuses system directories that it does not admit by name (Test-ClangX86Policy); the control
        # shows why.
        $clang = Join-Path $ClangToolchain 'bin\clang-cl.exe'
        if (-not (Test-Path -LiteralPath $clang)) { throw "$clang does not exist: -ClangToolchain takes the LLVM Windows toolchain of the recipes" }
        $target = '--target=i686-pc-windows-msvc'
        # clang-cl reads CL and _CL_ as cl.exe does: the MSVC policy (/external:W1) must not reach these compiles.
        $env:CL = $null; $env:_CL_ = $null
        Set-Content -Encoding ascii external/data.h 'int __stdcall data_value;'
        $clangCases = @(
            @{ Name='clang-data'; Ext='c'; Text='int __stdcall data_value;'; Diagnostic='-Werror,-Wignored-attributes'; Extra=@() },
            @{ Name='clang-cast'; Ext='c'; Text='int __cdecl f(int a) { return a; } typedef int (__stdcall *g)(int); g p = (g)f;'; Diagnostic='-Werror,-Wcast-calling-convention'; Extra=@() },
            @{ Name='clang-include'; Ext='c'; Text='#include <data.h>'; Diagnostic='-Werror,-Wignored-attributes'; Extra=@("/I$OutputDir/external") },
            @{ Name='clang-system-include'; Ext='c'; Text='#include <data.h>'; Diagnostic=$null; Extra=@("/external:I$OutputDir/external") },
            @{ Name='clang-pointer-c'; Ext='c'; Text=$bad; Diagnostic=$null; Extra=@() },
            @{ Name='clang-valid-c'; Ext='c'; Text=$good; Diagnostic=$null; Extra=@() },
            @{ Name='clang-valid-cpp'; Ext='cpp'; Text=$good; Diagnostic=$null; Extra=@() }
        )
        foreach ($case in $clangCases) {
            $src = "$($case.Name).$($case.Ext)"
            Set-Content -Encoding ascii $src $case.Text
            foreach ($enabled in $false, $true) {
                $switches = if ($enabled) { $clangPolicy } else { @() }
                $log = "$($case.Name)-$enabled.log"
                & $clang $target /nologo /c /W3 @switches @($case.Extra) $src "/Fo$($case.Name).obj" *> $log
                $code = $LASTEXITCODE
                $diagnostic = Get-Content -Raw $log
                if ($enabled -and $case.Diagnostic) {
                    if ($code -eq 0 -or -not $diagnostic.Contains($case.Diagnostic)) { throw "Expected $($case.Diagnostic) refusal: $log" }
                } elseif ($code -ne 0) { throw "Positive/control compile failed: $log" }
                $checks++
            }
        }
        foreach ($case in @(@{ Name='clang-redefinition'; Ext='cpp'; Text=$redef },
                            @{ Name='clang-redeclaration'; Ext='c'; Text='int __cdecl f(int); int __stdcall f(int a) { return a; }' })) {
            $src = "$($case.Name).$($case.Ext)"
            Set-Content -Encoding ascii $src $case.Text
            $log = "$($case.Name).log"
            & $clang $target /nologo /c /W0 $src "/Fo$($case.Name).obj" *> $log
            if ($LASTEXITCODE -eq 0) { throw "Expected a clang error without the policy: $log" }
            $checks++
        }
        # The pointer that MSVC refuses (C4229) is a stdcall pointer in clang: the caller does not pop the argument.
        Set-Content -Encoding ascii clang-pointer-call.c "$bad int call(callback p) { return p(1) + 1; }"
        & $clang $target /nologo /c /O2 /FA clang-pointer-call.c /Faclang-pointer-call.asm /Foclang-pointer-call.obj *> clang-pointer-call.log
        if ($LASTEXITCODE -ne 0) { throw 'clang-cl pointer call compile failed: clang-pointer-call.log' }
        $asm = Get-Content -Raw clang-pointer-call.asm
        if ($asm -notmatch 'call\s+dword ptr' -or $asm -match 'add\s+esp,\s*4') { throw 'clang-cl did not give a stdcall pointer: clang-pointer-call.asm' }
        $checks++
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }
Write-Output "$checks calling-convention checks passed (x86 compiler; no GPU work)"
exit 0
