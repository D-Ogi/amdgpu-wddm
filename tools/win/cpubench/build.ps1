param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\cpubench",
    [string]$KitVersion = '10.0.26100.0',
    # LLVM directory whose bin\ holds clang-cl.exe and lld-link.exe.
    [string]$Llvm = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64"
)

# Three builds of one source (README.md):
#   cpubench.exe        MSVC /O2, no /arch: SSE intrinsics legacy-encoded, AVX intrinsics VEX-encoded.
#   cpubench-avx2.exe   MSVC /O2 /arch:AVX2: everything VEX-encoded.
#   cpubench-clang.exe  clang-cl /O2 -mavx2 -mfma: the only build whose reg and lat lines are valid, because MSVC
#                       keeps the ten 256-bit chains of kernels.inc on the stack and clang keeps them in registers.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$clang = Join-Path $Llvm 'bin\clang-cl.exe'
# Both compilers are named by path and must exist; nothing falls back to whatever cl or clang is on PATH.
foreach ($tool in $cl, $clang) { if (-not (Test-Path -LiteralPath $tool)) { throw "compiler not found: $tool" } }
"cl        $cl"
# The banner is localized; only its version number (19.x.y) is read from it.
"          version $((& $cl 2>&1 | Out-String) -replace '(?s).*?\b(19\.\d+\.\d+(\.\d+)?)\b.*', '$1')"
"clang-cl  $clang"
"          $(& $clang --version | Select-Object -First 1)"
New-Item -ItemType Directory -Force $Out | Out-Null
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $here '..\..\..\..')).Path }
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Both compilers read the headers and libraries from the environment, so neither depends on a developer prompt.
$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), "$sdk\Include\$KitVersion\ucrt", "$sdk\Include\$KitVersion\um",
    "$sdk\Include\$KitVersion\shared") -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), "$sdkLib\ucrt\x64", "$sdkLib\um\x64") -join ';'

$src = Join-Path $here 'cpubench.c'
$builds = @(
    @{ Exe = 'cpubench.exe'; Tool = $cl; Args = @('/nologo', '/O2', '/W3', '/MT') },
    @{ Exe = 'cpubench-avx2.exe'; Tool = $cl; Args = @('/nologo', '/O2', '/W3', '/MT', '/arch:AVX2') },
    @{ Exe = 'cpubench-clang.exe'; Tool = $clang; Args = @('/nologo', '/O2', '/MT', '-mavx2', '-mfma', '-fuse-ld=lld') }
)
foreach ($b in $builds) {
    $obj = Join-Path $Out ([IO.Path]::ChangeExtension($b.Exe, '.obj'))
    & $b.Tool @($b.Args + @("/Fo$obj", "/Fe$(Join-Path $Out $b.Exe)", $src)) |
        ForEach-Object { if ($_ -notmatch '^\s*$|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$($b.Exe): compiler failed ($LASTEXITCODE)" }
}
Get-ChildItem $Out -Filter *.exe | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
