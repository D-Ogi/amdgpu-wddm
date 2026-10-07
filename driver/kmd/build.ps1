# Builds and test-signs bc250kmd (the M3 display-only miniport) without Visual Studio project files.
#
#   pwsh driver\kmd\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd
#
# Output: <Out>\package\{bc250kmd.sys, bc250kmd.inf, bc250kmd.cat, bc250-lab-test.cer}
#
# With -UmdStub <directory holding bc250umd.dll> a SECOND package is written next to the first, for the second
# run of M7 stage A: the same signed .sys, the stub DLL, and an INF generated from the repository's with the
# UserModeDriverName block live. Build the DLL first:
#
#   pwsh driver\umd-stub\build.ps1 -Kits ... -Out P:\BC-250\scratch\build\bc250umd
#   pwsh driver\kmd\build.ps1     -Kits ... -Out P:\BC-250\scratch\build\bc250kmd `
#                                 -UmdStub P:\BC-250\scratch\build\bc250umd
#
# Output: <Out>\package-umd\{bc250kmd.sys, bc250umd.dll, bc250kmd.inf, bc250kmd.cat, bc250-lab-test.cer}
#
# The two packages must carry the same driver, or the second run measures two changes instead of one, so the
# .sys is copied from the first package after it is signed and both SHA256 hashes are printed at the end.

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$UmdStub = '',
    [string]$KitVersion = '10.0.26100.0',
    [string]$CertSubject = 'CN=BC-250 lab test signing',
    [switch]$ExportCommandsOnly,
    [switch]$CompileOnly,           # compile and link, no quality gates, no catalog, no signature (a gate)
    [string]$QualityWorkspace = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$pkg = Join-Path $Out 'package'
$obj = Join-Path $Out 'obj'
if ((-not $ExportCommandsOnly) -and (Test-Path $obj)) { Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue }
# The debug information of an earlier build goes with the objects. /Brepro puts a hash of the debug
# information in the image, and cl appends to an existing cl.pdb instead of writing a fresh one, so a
# second build into the same directory lays that information out differently and the image changes
# although no source did. Measured 2026-10-06 on one head: a clean directory gives
# E440D4D7..., a second build into it 4A294E0D..., a third 698169EC... The package build of the respin
# removes its whole output directory first, which is why its driver reproduces; a caller that does not
# would get a different hash from the same sources, and the reproducibility claim of
# docs/design/reproducible-builds.md would be about the directory instead of the source.
if (-not $ExportCommandsOnly) {
    Remove-Item "$obj\cl.pdb", "$obj\*.ilk", "$Out\bc250kmd.pdb", "$Out\bc250kmd.map" `
        -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $pkg, $obj | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

# The second run's INF, generated from the repository's so that the two cannot drift. The repository INF is never
# modified. Two markers, both documented in bc250kmd.inf itself:
#
#   ;@UMD <line>            enabled here (the prefix is removed)
#   <line> ;@PLAIN-ONLY     disabled here (a ';' is put in front)
#
# It throws rather than write a package quietly missing the UserModeDriverName block, because such a package is
# byte for byte the first run's experiment while claiming to be the second one - the one failure mode of this
# whole switch that would not announce itself on the lab machine. Hence the checks against the RESULT below, not
# only against the markers: what matters is what came out, not that something was substituted.
function Write-UmdInf([string]$Source, [string]$Target) {
    $enabled = 0
    $disabled = 0
    $bumped = ''
    $lines = Get-Content -LiteralPath $Source | ForEach-Object {
        if ($_ -match '^;@UMD ?(.*)$') { $enabled++; $Matches[1] }
        # Same hardware id, same date, same version: on such a tie Windows keeps the driver it already has, and
        # run 2 would quietly be run 1 with a second package in the store. The last field goes up by one here, so
        # that the run 2 package outranks the plain one and the device's driver version says which run it is.
        elseif ($_ -match '^(DriverVer\s*=\s*[\d/]+,\d+\.\d+\.\d+\.)(\d+)\s*$') { $bumped = $Matches[1] + ([int]$Matches[2] + 1); $bumped }
        elseif ($_ -match ';@PLAIN-ONLY\s*$') { $disabled++; ";$_" }
        else { $_ }
    }
    if ($enabled -eq 0) { throw "$Source has no ;@UMD lines: the stage A run 2 block is gone or was renamed" }
    if ($disabled -eq 0) { throw "$Source has no ;@PLAIN-ONLY lines: the plain CopyFiles line is gone or was renamed" }
    if ($bumped -eq '') { throw "$Source has no DriverVer line of the form date,a.b.c.d: the run 2 package would tie with the plain one" }
    $text = $lines -join "`r`n"
    foreach ($want in '(?m)^\s*HKR,,\s*UserModeDriverName\b', '(?m)^\s*CopyFiles\s*=\s*Bc250_Files,\s*Bc250_UmdFiles\b',
                      '(?m)^\s*\[Bc250_UmdFiles\]', '(?m)^\s*bc250umd\.dll\s*=\s*1\b', '(?m)^\s*Bc250_UmdFiles\s*=\s*11\b') {
        if ($text -notmatch $want) { throw "the generated INF does not match $want : the markers in $Source moved" }
    }
    if ($text -match '(?m)^\s*CopyFiles\s*=\s*Bc250_Files\s*(;.*)?$') { throw 'the generated INF still copies only Bc250_Files' }
    # utf8NoBOM, which is how it was read: ascii would turn a proverb in a comment into question marks.
    Set-Content -LiteralPath $Target -Value ($text + "`r`n") -Encoding utf8NoBOM -NoNewline
    Write-Host "  bc250kmd.inf generated: $enabled lines enabled, $disabled commented out, $($bumped -replace '\s+', ' ')"
}

$env:INCLUDE = ''; $env:LIB = ''
$sources = (Get-ChildItem (Join-Path $here '*.c')).FullName
# M4, M5: AMD's imported code and the shim it compiles against (ADR 0002). Same flags; the imports get the warning
# disables documented in driver\amdgpu-import\PROVENANCE.md, from the build line, never by editing them.
$repo = Split-Path (Split-Path $here)
if(!$QualityWorkspace){$QualityWorkspace=if($env:BC250_ROOT){$env:BC250_ROOT}else{Split-Path $repo}}
$shimInc = @("/I$repo\driver\shim\include", "/I$repo\driver\amdgpu-import", "/I$repo\third_party\linux-amdgpu", "/I$repo\third_party\libdrm", '/DBC250_SHIM_KERNEL')
$shimSources = @("$repo\driver\shim\shim.c", "$repo\driver\shim\bc250_gmc.c", "$repo\driver\shim\bc250_gart.c", "$repo\driver\shim\bc250_pte.c", "$repo\driver\shim\bc250_psp.c")
# M5 second part: amdgpu's gfx/SDMA bring-up transcribed against AMD's imported tables. C4245: AMD's PACKET3() in the
# imported nvd.h is a signed int with bit 31 set (driver\shim\README.md).
# M6: bc250_ih.c, the interrupt ring (navi10_ih.c), is in this group for its include path.
$shimGfxSources = @('bc250_ring.c', 'bc250_gfx.c', 'bc250_sdma.c', 'bc250_sdma_copy.c', 'bc250_sdma_paging.c', 'bc250_sdma_virtual_ptes.c', 'bc250_nbio.c', 'bc250_irq.c', 'bc250_ih.c', 'bc250_dispatch.c', 'bc250_clock.c', 'bc250_smu.c', 'bc250_cu_mode.c', 'bc250_dpm.c', 'bc250_hwmon.c', 'bc250_fan.c', 'bc250_cpu.c', 'bc250_smu_metrics.c') | ForEach-Object { "$repo\driver\shim\$_" }
# Named, not globbed: only what this driver runs is compiled into it.
$importSources = @('gfxhub_v2_0.c', 'mmhub_v2_0.c', 'cyan_skillfish_reg_init.c', 'psp_v11_0_8.c') | ForEach-Object { "$repo\driver\amdgpu-import\$_" }
Write-Host 'compile'
$clFlags = @('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/we4013', '/we4020', '/we4024', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/Brepro',                      # no timestamp in the object files: the same sources must give the same bytes
    '/wd4201', '/wd4214',           # nameless unions and bit fields in the WDK's own headers
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um",
    "/Fo$obj\", "/Fd$obj\cl.pdb")
# Emit the exact per-file commands for targeted static analysis and build auditing.
$commands = @()
$groups = @(
    @{ Files=@($sources)+@($shimSources); Flags=$clFlags+$shimInc },
    @{ Files=@($shimGfxSources); Flags=$clFlags+$shimInc+@("/I$repo\driver\shim",'/TC','/wd4245') },
    @{ Files=@($importSources); Flags=$clFlags+$shimInc+@('/TC','/wd4244','/wd4701','/wd4100') }
)
foreach($group in $groups) {
    foreach($source in $group.Files) {
        $commands += @{ directory=$repo; file=$source; arguments=@((Join-Path $bin 'cl.exe'))+@($group.Flags)+@($source) }
    }
}
$commands | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out 'compile_commands.json') -Encoding utf8
if($ExportCommandsOnly) { Write-Host 'compile commands exported; no compilation or deployment'; return }
# -CompileOnly is the compiler as a gate: the same three cl.exe calls and the same link as a real package build,
# with no quality gates, no identity manifest, no catalog and no signature. It exists because the quality gate
# itself only EXPORTED these commands (quick.ps1 'kmd-commands' -ExportCommandsOnly), so a C error in
# driver/kmd or driver/shim reached nobody until somebody built a package. Running the gates here as well would
# be a loop: quick.ps1 is what calls this switch.
if(-not $CompileOnly) {
    & python (Join-Path $repo 'tools\quality\source_manifest.py') --repo $repo --out $Out --stage begin
    if($LASTEXITCODE -ne 0) { throw 'Source identity capture failed' }
    & (Join-Path $repo 'tools\quality\quick.cmd') $QualityWorkspace (Join-Path $Out 'quality') $repo
    if($LASTEXITCODE -ne 0) { throw 'Fast quality gates failed; KMD package not built' }
}

Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + $shimInc + $sources + $shimSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + $shimInc + @("/I$repo\driver\shim", '/TC', '/wd4245') + $shimGfxSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + $shimInc + @('/TC', '/wd4244', '/wd4701', '/wd4100') + $importSources)
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,10.00', '/ENTRY:DriverEntry', '/NODEFAULTLIB', '/RELEASE',
    '/DEBUG', '/OPT:REF', '/OPT:ICF', '/MACHINE:X64', "/LIBPATH:$wdk\Lib\$KitVersion\km\x64",
    '/Brepro', '/PDBALTPATH:%_PDB%',  # a content hash instead of a timestamp, and the PDB by name, not by path
    'displib.lib', 'ntoskrnl.lib', 'hal.lib', 'bufferoverflowfastfailk.lib', 'libcntpr.lib', 'ntstrsafe.lib',
    "/OUT:$pkg\bc250kmd.sys", "/PDB:$Out\bc250kmd.pdb", "/MAP:$Out\bc250kmd.map") +
    (Get-ChildItem "$obj\*.obj" | Sort-Object -Property Name).FullName)   # fixed order: /OPT:ICF folds by input order
Copy-Item "$pkg\bc250kmd.sys" (Join-Path $Out 'bc250kmd.unsigned.sys') -Force

# What does the prologue of each function take off rsp? A kernel thread has 24 KB and dxgmms2 has already
# spent some of it when it calls us (facts M104: a 0x5B00-byte local bugchecked 0x50 in nt!_chkstk).
Write-Host 'stack budget'
& python (Join-Path $repo 'tools\win\stackbudget.py') "$pkg\bc250kmd.sys" '--map' "$Out\bc250kmd.map"
if ($LASTEXITCODE -ne 0) { throw 'stack budget: a function allocates too much stack for a kernel thread (see above)' }

if($CompileOnly) {
    # The unsigned bytes, so that a gate run can be compared with the package build of the same tree by hand.
    '  {0}  bc250kmd.unsigned.sys' -f (Get-FileHash (Join-Path $Out 'bc250kmd.unsigned.sys') -Algorithm SHA256).Hash
    Write-Host 'compiled and linked; no catalog, no signature, no deployment'
    return
}

Copy-Item (Join-Path $here 'bc250kmd.inf') $pkg -Force
Write-Host 'catalog'
Invoke-Tool "$wdk\bin\$KitVersion\x86\Inf2Cat.exe" @("/driver:$pkg", '/os:10_X64')

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) { throw "test certificate '$CertSubject' not found: run tools\win\bc250rd\build.ps1 once, it creates it" }
Export-Certificate -Cert $cert -FilePath "$pkg\bc250-lab-test.cer" | Out-Null
$signtool = Join-Path $sdk "bin\$KitVersion\x64\signtool.exe"
Write-Host 'sign'
foreach ($f in 'bc250kmd.sys', 'bc250kmd.cat') { Invoke-Tool $signtool @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, "$pkg\$f") }
Get-ChildItem $pkg -File | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }

# ---- the second stage A package: same driver, a user-mode driver name, the stub DLL --------------------------
$pkgUmd = $null
if ($UmdStub) {
    $dll = Join-Path $UmdStub 'bc250umd.dll'
    if (-not (Test-Path -LiteralPath $dll)) { throw "-UmdStub $UmdStub holds no bc250umd.dll: build driver\umd-stub\build.ps1 first" }
    $pkgUmd = Join-Path $Out 'package-umd'
    New-Item -ItemType Directory -Force $pkgUmd | Out-Null
    # Named, not wildcarded: a stale .cat left here would be catalogued into the new one, and this is the only
    # directory in the build that is not rebuilt from nothing.
    foreach ($f in 'bc250kmd.sys', 'bc250kmd.inf', 'bc250kmd.cat', 'bc250umd.dll', 'bc250-lab-test.cer') {
        Remove-Item (Join-Path $pkgUmd $f) -Force -ErrorAction SilentlyContinue
    }
    Write-Host 'umd package'
    # The signed binary itself, copied: the second run must differ from the first in the INF and the DLL only.
    Copy-Item "$pkg\bc250kmd.sys" $pkgUmd -Force
    Copy-Item $dll $pkgUmd -Force
    Write-UmdInf (Join-Path $here 'bc250kmd.inf') (Join-Path $pkgUmd 'bc250kmd.inf')
    # Inf2Cat before the certificate is put there: the .cer is not a packaged file and Inf2Cat would object to it.
    # Signing the .sys does not disturb the catalog, which hashes the PE without its certificate table - which is
    # also why the plain package above catalogues before it signs.
    Invoke-Tool "$wdk\bin\$KitVersion\x86\Inf2Cat.exe" @("/driver:$pkgUmd", '/os:10_X64')
    Invoke-Tool $signtool @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, "$pkgUmd\bc250kmd.cat")
    Copy-Item "$pkg\bc250-lab-test.cer" $pkgUmd -Force
    Write-Host "umd package: $pkgUmd"
    Get-ChildItem $pkgUmd -File | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
}

# The driver both packages install, so that "same binary" is a hash and not a claim.
foreach ($p in @($pkg, $pkgUmd)) {
    if ($p) { '  {0}  {1}\bc250kmd.sys' -f (Get-FileHash "$p\bc250kmd.sys" -Algorithm SHA256).Hash, (Split-Path -Leaf $p) }
}

# Bind the signed artifacts to the exact inputs captured before compilation.
$manifestArgs = @((Join-Path $repo 'tools\quality\source_manifest.py'), '--repo', $repo, '--out', $Out, '--stage', 'end', '--package', $pkg)
if($pkgUmd) { $manifestArgs += @('--package', $pkgUmd) }
& python @manifestArgs
if($LASTEXITCODE -ne 0) { throw 'Source/artifact identity verification failed; do not deploy this package' }
