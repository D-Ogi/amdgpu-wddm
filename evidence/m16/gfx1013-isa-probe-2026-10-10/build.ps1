# Builds dotprobe.exe and its gfx1013 code object, and verifies that the code object holds
# exactly the instructions the probe claims to test.
#
#   pwsh scratch\gfx1013-dot-probe\build.ps1
#   pwsh scratch\gfx1013-dot-probe\build.ps1 -Out P:\bc-250\scratch\build\gfx1013-dot
#
# Why five stages instead of one clang call. The frontend refuses the dot builtins for gfx1013
# ("needs target feature dot1-insts"), so every instruction under test is inline assembly. The
# integrated assembler of a one-call build would refuse the same instructions at the next step,
# for the same reason. Two other routes were measured and rejected:
#
#   * -Xclang -target-feature -Xclang +dot1-insts on the device pass. MEASURED 2026-10-10 with
#     clang 22.1.8: the compilation succeeds, exits 0, and the kernel disappears from the code
#     object ("amdhsa.kernels: []"). A build that silently drops the kernel must not be used.
#   * compiling for gfx1012 and retargeting the ELF. Not needed: the encodings are identical
#     (stage 6 proves it), so the assembler alone can be told to accept them.
#
# So the device text is produced for gfx1013 by clang (-S, which does not assemble), and the
# assembler is told to accept the dot opcodes (llvm-mc -mattr=+dot1-insts ...). The result is a
# real gfx1013 code object: ELF e_flags 0x142, mach gfx1013, code object v6, the same values
# clang itself writes for this target.

param(
    [string]$Root = 'P:\bc-250',
    [string]$Out = '',
    [string]$Clang = '',
    [switch]$SkipHost
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\gfx1013-dot' }
if (-not $Clang) { $Clang = Join-Path $Root 'toolchain\llvm-amdgpu-22.1.8\mingw64\bin' }
$hipInclude = Join-Path $Root 'scratch\m16-hip\step3\wt\compute\hip\include'
$implibDir = Join-Path $Root 'scratch\build\m16-step3-l2'

$clangExe = Join-Path $Clang 'clang.exe'
$clangxx = Join-Path $Clang 'clang++.exe'
$llvmmc = Join-Path $Clang 'llvm-mc.exe'
$lld = Join-Path $Clang 'ld.lld.exe'
$bundler = Join-Path $Clang 'clang-offload-bundler.exe'
$objdump = Join-Path $Clang 'llvm-objdump.exe'
$readelf = Join-Path $Clang 'llvm-readelf.exe'
foreach ($t in @($clangExe, $clangxx, $llvmmc, $lld, $bundler, $objdump, $readelf)) {
    if (-not (Test-Path -LiteralPath $t)) { throw "$t not found" }
}
if (-not (Test-Path -LiteralPath $hipInclude)) { throw "$hipInclude not found" }
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$source = Join-Path $here 'dotprobe.hip'
$devAsm = Join-Path $Out 'dotprobe.gfx1013.s'
$devObj = Join-Path $Out 'dotprobe.gfx1013.o'
$devCo = Join-Path $Out 'dotprobe.gfx1013.co'
$fatbin = Join-Path $Out 'dotprobe.fatbin'
$exe = Join-Path $Out 'dotprobe.exe'

# The instructions the probe tests, with the LLVM target feature that gates each one on GFX10.1
# and the opcode of its GFX10 encoding (llvm/lib/Target/AMDGPU/VOP3PInstructions.td and
# VOP2Instructions.td of LLVM 23.1.2). Every one is expected exactly once in the code object.
$instructions = @(
    @{ text = 'v_pk_fma_f16';       feature = '';             note = 'control, VOP3P 0x0e' }
    @{ text = 'v_fma_mix_f32';      feature = '';             note = 'control, VOP3P 0x20' }
    @{ text = 'v_mul_i32_i24_sdwa'; feature = '';             note = 'control, VOP2 0x09 + SDWA' }
    @{ text = 'v_dot4_i32_i8';      feature = 'dot1-insts';   note = 'VOP3P 0x16' }
    @{ text = 'v_dot2_f32_f16';     feature = 'dot10-insts';  note = 'VOP3P 0x13' }
    @{ text = 'v_dot4_u32_u8';      feature = 'dot7-insts';   note = 'VOP3P 0x17' }
    # These two also appear in the object as the hand-encoded word of the slot they occupy, so
    # the disassembly holds each of them twice. That the raw word decodes back to the mnemonic
    # is itself a check that the word is right.
    @{ text = 'v_dot4c_i32_i8';     feature = 'dot6-insts';   note = 'VOP2 0x0d'; dis = 2 }
    @{ text = 'v_dot2c_f32_f16';    feature = 'dot5-insts';   note = 'VOP2 0x02'; dis = 2 }
    @{ text = 'v_dot2_i32_i16';     feature = 'dot2-insts';   note = 'VOP3P 0x14' }
    @{ text = 'v_dot2_u32_u16';     feature = 'dot2-insts';   note = 'VOP3P 0x15' }
    @{ text = 'v_dot8_i32_i4';      feature = 'dot1-insts';   note = 'VOP3P 0x18' }
    @{ text = 'v_dot8_u32_u4';      feature = 'dot7-insts';   note = 'VOP3P 0x19' }
)
$mattr = '+dot1-insts,+dot2-insts,+dot5-insts,+dot6-insts,+dot7-insts,+dot10-insts'

# The VOP2 slot probe. gfx1013 is documented to have v_min_f32 (VOP2 0x00f) and v_max_f32
# (0x010); it is the two slots below them, 0x00d and 0x00e, that SI and CI used for
# V_MIN_LEGACY_F32 and V_MAX_LEGACY_F32 and that GFX10 leaves empty. 0x00d and 0x002 are where
# GFX10.1 put the two dot accumulate forms. The probe reaches 0x00e by writing the word out by
# hand, because no GFX10 mnemonic assembles to it.
#
# These two run through the ordinary assembler, and objdump prints them with an _e32 suffix.
# Each appears twice in the object: the mnemonic case, and the hand-encoded control for the raw
# route that must produce the same instruction.
$yardsticks = @(
    @{ src = 'v_min_f32'; dis = 'v_min_f32_e32'; word = '1E0A1308'; mattr = '' }
    @{ src = 'v_max_f32'; dis = 'v_max_f32_e32'; word = '200A1308'; mattr = '' }
)

# The words the kernel writes out by hand, with vdst = v5, src0 = v8, vsrc1 = v9. `check` is the
# instruction whose assembled encoding must equal the word, or '' when no mnemonic exists and
# only the field arithmetic can be checked.
$rawWords = @(
    @{ word = '1E0A1308'; op = 0x00f; check = 'v_min_f32';      note = 'the raw route control' }
    @{ word = '200A1308'; op = 0x010; check = 'v_max_f32';      note = 'the raw route control' }
    @{ word = '1A0A1308'; op = 0x00d; check = 'v_dot4c_i32_i8'; note = 'the v_dot4c slot' }
    @{ word = '040A1308'; op = 0x002; check = 'v_dot2c_f32_f16'; note = 'the v_dot2c slot' }
    @{ word = '1C0A1308'; op = 0x00e; check = '';               note = 'empty on GFX10' }
)

Write-Host "dotprobe build, out $Out"

# ---------------------------------------------------------------------------------------------
# Stage 1: the device text for gfx1013. clang -S does not assemble, so the inline assembly of
# the dot instructions passes through as text.
# ---------------------------------------------------------------------------------------------
& $clangxx '-x' 'hip' '--offload-arch=gfx1013' '--offload-device-only' '--no-gpu-bundle-output' `
    '-nogpuinc' '-nogpulib' '-O2' '-std=c++17' "-I$hipInclude" '-S' '-o' $devAsm $source
if ($LASTEXITCODE -ne 0) { throw "stage 1 (clang -S for gfx1013) failed ($LASTEXITCODE)" }
Write-Host "  1. device text $((Get-Item $devAsm).Length) bytes"

# Each instruction must appear exactly once in the device text: one kernel per instruction, and
# no copy the optimiser made of a kernel body.
$asmText = Get-Content -LiteralPath $devAsm
foreach ($ins in $instructions) {
    $n = ($asmText | Select-String -SimpleMatch -Pattern "`t$($ins.text) " ).Count
    if ($n -ne 1) { throw "the device text holds $($ins.text) $n times, expected exactly 1" }
}
Write-Host "  1. each of the $($instructions.Count) instructions appears exactly once"

foreach ($y in $yardsticks) {
    $n = ($asmText | Select-String -SimpleMatch -Pattern "`t$($y.src) " ).Count
    if ($n -ne 1) { throw "the device text holds $($y.src) $n times, expected exactly 1" }
}
foreach ($r in $rawWords) {
    $lit = ".long 0x$($r.word.ToLower())"
    $n = ($asmText | Select-String -SimpleMatch -Pattern $lit).Count
    if ($n -ne 1) { throw "the device text holds '$lit' $n times, expected exactly 1" }
}
Write-Host "  1. $($yardsticks.Count) yardstick instructions and $($rawWords.Count) hand-encoded words, once each"

# The field arithmetic of every hand-written word, against the assembler. VOP2 on GFX10:
# src0 [8:0], vsrc1 [16:9], vdst [24:17], opcode [30:25]. With vdst = v5, src0 = v8 (256 + 8)
# and vsrc1 = v9 the constant part is 0x000a1308. Where a mnemonic exists for the slot, the
# assembler's own encoding of that mnemonic with those registers must equal the word, so the
# word is not merely self-consistent: it is the assembler's.
foreach ($r in $rawWords) {
    $want = ('{0:X8}' -f ((($r.op -shl 25) -bor 0x000a1308)))
    if ($want -ne $r.word) { throw "word $($r.word): opcode $($r.op) gives $want" }
    if (-not $r.check) { continue }
    $tmp = Join-Path $Out 'raw.s'
    Set-Content -LiteralPath $tmp -Value "$($r.check) v5, v8, v9" -Encoding ascii
    $out1 = (& $llvmmc '-triple=amdgcn-amd-amdhsa' '-mcpu=gfx1013' "-mattr=$mattr" `
        '-show-encoding' $tmp 2>&1) -join "`n"
    if ($out1 -notmatch 'encoding:\s+\[([^\]]+)\]') {
        throw "cannot assemble '$($r.check) v5, v8, v9': $out1"
    }
    # The encoding comment is little-endian bytes; the word is the same bytes, reversed.
    $b = ($Matches[1] -replace '\s', '') -split ',' | Where-Object { $_ }
    $asmWord = (($b | ForEach-Object { $_ -replace '^0x', '' })[3..0] -join '').ToUpper()
    if ($asmWord -ne $r.word) {
        throw "$($r.check) v5, v8, v9 assembles to $asmWord, the kernel writes $($r.word)"
    }
}
Write-Host "  1. every hand-encoded word matches the VOP2 field arithmetic and the assembler"

# ---------------------------------------------------------------------------------------------
# Stage 2: assemble for gfx1013 with the Dot features forced on the assembler only.
# ---------------------------------------------------------------------------------------------
& $llvmmc '-triple=amdgcn-amd-amdhsa' '-mcpu=gfx1013' "-mattr=$mattr" '-filetype=obj' `
    '-o' $devObj $devAsm
if ($LASTEXITCODE -ne 0) { throw "stage 2 (llvm-mc) failed ($LASTEXITCODE)" }
Write-Host "  2. object $((Get-Item $devObj).Length) bytes"

# ---------------------------------------------------------------------------------------------
# Stage 3: link the code object. -m elf64_amdgpu is required: without it this lld refuses the
# AMDGPU relocatable with "unknown file type" (MEASURED 2026-10-10).
# ---------------------------------------------------------------------------------------------
& $lld '-m' 'elf64_amdgpu' '-shared' '-o' $devCo $devObj
if ($LASTEXITCODE -ne 0) { throw "stage 3 (ld.lld) failed ($LASTEXITCODE)" }
$hdr = (& $readelf '-h' $devCo) -join "`n"
if ($hdr -notmatch 'gfx1013') { throw "the code object does not name gfx1013" }
if ($hdr -notmatch 'ABI Version:\s+4') { throw "the code object is not code object v6 (ABI version 4)" }
Write-Host "  3. code object $((Get-Item $devCo).Length) bytes, gfx1013, code object v6"

# ---------------------------------------------------------------------------------------------
# Stage 4: the offload bundle the HIP runtime registers. The same two targets the committed
# fixture carries (compute/hip/tests/data/hip_test_kernels.gfx1013.fatbin).
# ---------------------------------------------------------------------------------------------
$emptyHost = Join-Path $Out 'host-empty.o'
Set-Content -LiteralPath $emptyHost -Value '' -NoNewline -Encoding ascii
& $bundler '--type=o' '--bundle-align=4096' `
    '--targets=host-x86_64-unknown-linux-gnu-,hipv4-amdgcn-amd-amdhsa--gfx1013' `
    "--input=$emptyHost" "--input=$devCo" "--output=$fatbin"
if ($LASTEXITCODE -ne 0) { throw "stage 4 (clang-offload-bundler) failed ($LASTEXITCODE)" }
Write-Host "  4. fatbin $((Get-Item $fatbin).Length) bytes"

# ---------------------------------------------------------------------------------------------
# Stage 5: the host program, with the fatbin of stage 4 instead of a device pass of its own.
# ---------------------------------------------------------------------------------------------
if ($SkipHost) {
    Write-Host '  5. skipped (-SkipHost)'
} else {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    if (-not $vs) { throw 'no Visual Studio toolset found (vswhere returned nothing)' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
    $savedPath = $env:PATH
    & cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^(INCLUDE|LIB|LIBPATH|UCRTVersion|WindowsSdkDir|WindowsSDKVersion)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
        if ($_ -match '^Path=(.*)$') { $env:PATH = "$Clang;$($Matches[1])" }
    }
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $implibDir 'amdhip64.lib'))) {
            throw "amdhip64.lib not found in $implibDir (build compute\hip\build-runtime.ps1 first)"
        }
        & $clangxx '-x' 'hip' '--cuda-host-only' '--offload-arch=gfx1013' `
            '--target=x86_64-pc-windows-msvc' '-nogpuinc' '-nogpulib' '-O2' '-std=c++17' `
            "-I$hipInclude" '-Xclang' '-fcuda-include-gpubinary' '-Xclang' $fatbin `
            '-o' $exe $source "-L$implibDir"
        if ($LASTEXITCODE -ne 0) { throw "stage 5 (host compile) failed ($LASTEXITCODE)" }
    } finally {
        $env:PATH = $savedPath
    }
    $imports = & $objdump '--private-headers' $exe | Select-String 'DLL Name'
    if (-not ($imports -match 'amdhip64.dll')) { throw 'dotprobe.exe does not import amdhip64.dll' }
    Write-Host "  5. dotprobe.exe $((Get-Item $exe).Length) bytes, imports amdhip64.dll"
}

# ---------------------------------------------------------------------------------------------
# Stage 6: the encoding gate. Every instruction in the code object must be the one the probe
# names, exactly once, and its four bytes must equal what the assembler emits for gfx1012 and
# for gfx1030, which are parts that have the Dot features by their own definition. This is what
# makes a result on the hardware mean something: the bytes are not ours, they are the bytes a
# supported part would receive.
# ---------------------------------------------------------------------------------------------
$dis = Join-Path $Out 'dotprobe.gfx1013.dis'
& $objdump '-d' "--mcpu=gfx1013" "--mattr=$mattr" $devCo | Set-Content -LiteralPath $dis -Encoding ascii
$disText = Get-Content -LiteralPath $dis
$table = @()
foreach ($ins in $instructions) {
    $lines = @($disText | Where-Object { $_ -match "^\s*$([regex]::Escape($ins.text))\s" })
    $wantDis = if ($ins.ContainsKey('dis')) { $ins.dis } else { 1 }
    if ($lines.Count -ne $wantDis) {
        throw "the disassembly holds $($ins.text) $($lines.Count) times, expected exactly $wantDis"
    }
    # "  v_dot4_i32_i8 v3, v1, v2, v3   // 0000000016A8: CC164003 1C0E0501"
    if ($lines[0] -notmatch '//\s+[0-9A-Fa-f]+:\s+([0-9A-Fa-f ]+)$') {
        throw "cannot read the encoding of $($ins.text) from the disassembly"
    }
    $bytes = $Matches[1].Trim()
    $operands = ($lines[0] -split '//')[0].Trim()
    $table += @{ text = $ins.text; feature = $ins.feature; note = $ins.note
                 operands = $operands; bytes = $bytes }
}

# The same operand text, assembled for the three targets.
$cmp = Join-Path $Out 'encoding-compare.txt'
$report = @()
$report += "# gfx1013 against gfx1012 and gfx1030: the same instruction text, the same bytes."
$report += "# gfx1013 is assembled with -mattr=$mattr; the other two need no attribute."
$report += ''
$ok = 0
foreach ($row in $table) {
    $tmp = Join-Path $Out 'one.s'
    Set-Content -LiteralPath $tmp -Value $row.operands -Encoding ascii
    $enc = @{}
    foreach ($pair in @(@('gfx1013', $mattr), @('gfx1012', ''), @('gfx1030', ''))) {
        $args = @('-triple=amdgcn-amd-amdhsa', "-mcpu=$($pair[0])", '-show-encoding', $tmp)
        if ($pair[1]) { $args = @($args[0], $args[1], "-mattr=$($pair[1])") + $args[2..3] }
        $out2 = (& $llvmmc @args 2>&1) -join "`n"
        if ($out2 -notmatch 'encoding:\s+\[([^\]]+)\]') {
            throw "$($pair[0]) does not assemble '$($row.operands)': $out2"
        }
        $enc[$pair[0]] = $Matches[1] -replace '\s', ''
    }
    $same = ($enc['gfx1013'] -eq $enc['gfx1012']) -and ($enc['gfx1013'] -eq $enc['gfx1030'])
    if (-not $same) {
        throw "$($row.text): gfx1013 $($enc['gfx1013']) gfx1012 $($enc['gfx1012']) gfx1030 $($enc['gfx1030'])"
    }
    $ok++
    $report += "$($row.text)"
    $report += "  operands      $($row.operands)"
    $report += "  in the object $($row.bytes)   ($($row.note))"
    $report += "  gfx1013       $($enc['gfx1013'])"
    $report += "  gfx1012       $($enc['gfx1012'])"
    $report += "  gfx1030       $($enc['gfx1030'])"
    $report += "  feature       $(if ($row.feature) { $row.feature } else { 'none: gfx1013 has this instruction' })"
    $report += ''
}
Set-Content -LiteralPath $cmp -Value $report -Encoding ascii
Write-Host "  6. $ok of $($instructions.Count) instructions: the gfx1013 bytes equal the gfx1012 and gfx1030 bytes"
Write-Host "  6. $cmp"

# ---------------------------------------------------------------------------------------------
# Stage 7: the slot probe survives the whole pipeline. Each hand-written word must still be in
# the linked code object as that exact encoding, and the two yardstick instructions must be
# there twice each: once as the compiler emitted them, once as the hand-encoded control.
# ---------------------------------------------------------------------------------------------
foreach ($r in $rawWords) {
    $n = @($disText | Where-Object { $_ -match "(?i)\b$($r.word)\b" }).Count
    if ($n -lt 1) {
        throw "the code object does not hold the word $($r.word) ($($r.note))"
    }
    Write-Host "  7. word $($r.word) VOP2 opcode 0x$('{0:x3}' -f $r.op): $n in the object, $($r.note)"
}
foreach ($y in $yardsticks) {
    $n = @($disText | Where-Object { $_ -match "^\s*$([regex]::Escape($y.dis))\s" }).Count
    if ($n -ne 2) {
        throw "the code object holds $($y.dis) $n times, expected 2 (the mnemonic and the raw control)"
    }
}
Write-Host "  7. both yardstick instructions appear twice: the mnemonic case and its raw control"

foreach ($f in @($devCo, $fatbin, $exe)) {
    if (Test-Path -LiteralPath $f) {
        $h = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash
        Write-Host ("  {0,-22} {1,8} bytes  {2}" -f (Split-Path -Leaf $f), (Get-Item $f).Length, $h.Substring(0, 8))
    }
}
Write-Host 'dotprobe build ok'
