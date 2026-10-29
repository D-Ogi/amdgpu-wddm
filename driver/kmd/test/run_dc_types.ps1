param([Parameter(Mandatory)][string]$Root,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
$clang="$Root\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$msvc=Join-Path $crt.FullName 'bin\Hostx64\x64\cl.exe'
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
$env:INCLUDE=''; $env:LIB=''
New-Item -ItemType Directory -Force $Out | Out-Null
& $clang --version
if ($LASTEXITCODE) {throw 'Explicit clang-cl unavailable'}
# MSVC is used only to test the selected KMD compiler, never as a host fallback.
$cases=@(
    @{Name='clang-host'; Compiler=$clang; Flags=@('-fuse-ld=lld')},
    @{Name='clang-kernel-types'; Compiler=$clang; Flags=@('-fuse-ld=lld','/DBC250_SHIM_KERNEL')},
    @{Name='msvc-kernel-types'; Compiler=$msvc; Flags=@('/DBC250_SHIM_KERNEL')},
    @{Name='msvc-c11-kernel-types'; Compiler=$msvc; Flags=@('/DBC250_SHIM_KERNEL','/std:c11')}
)
$header=[IO.File]::ReadAllText("$repo\driver\shim\bc250_dc_types.h")
if ([regex]::Matches($header,[regex]::Escape('typedef _Bool bool;')).Count -ne 1) {throw 'Boolean mutation anchor changed'}
$mutant=Join-Path $Out 'mutant'
New-Item -ItemType Directory -Force $mutant | Out-Null
[IO.File]::WriteAllText("$mutant\bc250_dc_types.h",$header.Replace('typedef _Bool bool;','typedef unsigned char bool;'))
foreach($case in $cases) {
    foreach($negative in @($false,$true)) {
        if($negative -and $case.Name -eq 'clang-host') {continue}
        $name=$case.Name + $(if($negative){'-uchar'}else{''})
        $flags=@($case.Flags)
        if($negative){$flags+="/I$mutant"}
        & $case.Compiler @flags /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\shim" `
            "/I$($crt.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" `
            "/Fo$Out\$name.obj" "/Fe$Out\$name.exe" "$PSScriptRoot\dc_types_test.c" `
            /link "/LIBPATH:$($crt.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
        if($LASTEXITCODE){throw "DC types compile failed: $name"}
        $lines=& "$Out\$name.exe"
        $code=$LASTEXITCODE
        if($negative) {
            if(!$code -or ($lines -join "`n") -notmatch 'FAIL line'){throw "Boolean mutation survived: $name"}
            Write-Output "$name`: old unsigned-char conversion rejected"
        } else {
            $lines | Write-Output
            if($code){throw "DC types tests failed: $name"}
            Write-Output "$name`: PASS"
        }
    }
}
