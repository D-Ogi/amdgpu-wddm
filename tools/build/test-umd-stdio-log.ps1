param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
# Host gate of the diagnostic sink (driver/umd/d3d12/stdio-log.h): which sink a process gets (AMDGPU_WDDM_LOG
# against the control app's timed "Record game logs" switch), the bounds of a switched-on recording (the instant,
# the size cap, the one last line), the file name the switch builds, and the refusal of a planted name, in the
# release and the lab flavour.
# The D3D12 shell, the D3D11 shell and the router builds run it before they produce a DLL.
# Everything the test writes stays below -OutputDir; HKEY_LOCAL_MACHINE is replaced by a private hive for the test
# process only, and ProgramData points below the same directory.
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells. -Compiler msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy). The script writes
# compiler.json into -OutputDir and prints the compiler path and version.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-stdio-log' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/DNOMINMAX') @() $Arch @($repo)
    # Both flavours, whichever one this build produces (BD-167): the release sink reads AMDGPU_WDDM_LOG only as the
    # carrier of the timed switch, the lab sink honours every value. Each one runs in a directory of its own.
    foreach ($f in @(@{ Name = 'release'; Define = @() }, @{ Name = 'lab'; Define = @('/DAMDGPU_WDDM_LAB_FLAVOUR=1') })) {
        $dir=Join-Path $OutputDir $f.Name
        New-Item -ItemType Directory -Force $dir | Out-Null
        Push-Location $dir
        try {
            Invoke-Bc250UmdCompiler $cc @flags @($f.Define) /Fe:stdio-log-test.exe `
                "$repo\driver\umd\d3d12\stdio-log-test.cpp" /link advapi32.lib
            if ($LASTEXITCODE -ne 0) { throw "Diagnostic sink test build failed ($($f.Name) flavour)" }
            & .\stdio-log-test.exe
            if ($LASTEXITCODE -ne 0) { throw "Diagnostic sink tests failed ($($f.Name) flavour)" }
        } finally { Pop-Location }
    }
} finally { Restore-ProcessEnvironment $saved }
