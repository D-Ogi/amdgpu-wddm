# Builds bd167probe.exe (BD-167 fix 9): headers and import libraries from the SDK NuGet packages under -Kits, clang-cl
# and lld-link of LLVM 23.1.4 (tools\build\umd-compiler.ps1), the same recipe as tools\win\clprobe.
#
#   pwsh tools\win\bd167probe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget [-Out <dir>]
#
# Before the compile it checks the register: `regcalc.py lookup mmSCRATCH_REG0` must give the BAR5 offset that
# driver/kmd/regs.generated.h names BC250_REG_GC_SCRATCH_REG0, and that offset must be on g_MmioWriteAllow there.
# Ends with the SHA-256 of bd167probe.exe and writes SHA256SUMS.txt next to it.
param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$Kits = [IO.Path]::GetFullPath($Kits)
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$workspace = Split-Path (Split-Path $Kits -Parent) -Parent
if (-not $Out) { $Out = Join-Path $workspace 'scratch\build\bd167probe' }
$Out = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force $Out, "$Out\tmp" | Out-Null
$env:TEMP = "$Out\tmp"; $env:TMP = $env:TEMP

# The register: regcalc (repo rule 1) against the generated header the program includes.
$lookup = (& python (Join-Path $repo 'tools\regcalc\regcalc.py') lookup mmSCRATCH_REG0) -join ' '
if ($LASTEXITCODE -or $lookup -notmatch 'BAR5\+(0x[0-9A-Fa-f]+)') { throw "regcalc lookup mmSCRATCH_REG0 failed: $lookup" }
$fromRegcalc = [Convert]::ToUInt32($Matches[1].Substring(2), 16)
$regs = Get-Content -LiteralPath (Join-Path $repo 'driver\kmd\regs.generated.h') -Raw
if ($regs -notmatch '#define BC250_REG_GC_SCRATCH_REG0\s+0x([0-9A-Fa-f]+)ul') { throw 'BC250_REG_GC_SCRATCH_REG0 is not in regs.generated.h' }
$fromHeader = [Convert]::ToUInt32($Matches[1], 16)
if ($fromRegcalc -ne $fromHeader) { throw ('GC.SCRATCH_REG0: regcalc 0x{0:X5}, regs.generated.h 0x{1:X5}' -f $fromRegcalc, $fromHeader) }
if ($regs -notmatch '(?s)g_MmioWriteAllow\[BC250_MMIO_WRITE_ALLOW_COUNT\] = \{(.*?)\};') { throw 'g_MmioWriteAllow is not in regs.generated.h' }
if ($Matches[1] -notmatch ('0x{0:x5}ul' -f $fromHeader)) { throw ('GC.SCRATCH_REG0 0x{0:X5} is not on g_MmioWriteAllow' -f $fromHeader) }
'GC.SCRATCH_REG0 BAR5+0x{0:X5}: regcalc and regs.generated.h agree, on the write allow list' -f $fromHeader

. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE = "$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB = "$($msvc.FullName)\lib\x64;$libs\ucrt\x64;$libs\um\x64"
$env:PATH = "$($msvc.FullName)\bin\Hostx64\x64;$env:PATH"
$cc = Resolve-Bc250UmdCompiler $workspace 'clang-cl' '' $Out 'x64'
$repro = Get-Bc250ReproducibleFlags 'clang-cl'
$flags = @(Get-Bc250HostTestFlags 'clang-cl' @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', '/Brepro') @() 'x64' @($repo)) +
    $repro.Compile + '/clang:-fuse-ld=lld'
Push-Location $Out
try {
    Invoke-Bc250UmdCompiler $cc @flags '/Fo.\' '/Febd167probe.exe' (Join-Path $PSScriptRoot 'bd167probe.c') `
        /link /Brepro '/PDBALTPATH:%_PDB%' /DYNAMICBASE /NXCOMPAT @($repro.Link) kernel32.lib gdi32.lib
    if ($LASTEXITCODE) { throw 'bd167probe compile failed' }
} finally { Pop-Location }
Remove-Item -LiteralPath "$Out\tmp" -Recurse -Force -ErrorAction SilentlyContinue
$sha = (Get-FileHash -LiteralPath "$Out\bd167probe.exe" -Algorithm SHA256).Hash
"$($sha.ToLower()) *bd167probe.exe" | Set-Content -LiteralPath "$Out\SHA256SUMS.txt" -Encoding ascii
'{0,9}  bd167probe.exe  sha256 {1}' -f (Get-Item "$Out\bd167probe.exe").Length, $sha
