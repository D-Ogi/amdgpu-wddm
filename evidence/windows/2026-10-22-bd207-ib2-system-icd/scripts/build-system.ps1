#Requires -Version 7.0
# BD-207: the system x64 Vulkan ICD (radv-system, clang-cl, variants release,isa: the tester.32 recipe of
# scratch\train\b35-build\wt) built from this lane's work tree, one flavour per build directory, untrimmed so that
# the winsys host tests can link against its objects. Then the host tests of the lane run on each build.
#   pwsh -NoProfile -File build-system.ps1 [-Flavours lab,release] [-Source <tree>] [-Tag <name>]
param(
    [string[]]$Flavours = @('lab', 'release'),
    [string]$Source = 'P:\BC-250\scratch\bd207-ib2\wt',
    [string]$Recipe = 'P:\BC-250\scratch\train\b35-build\wt',
    [string]$OutRoot = 'P:\BC-250\scratch\bd207-ib2',
    # Names the build directories: b-system-<tag>-<flavour>.
    [string]$Tag = 'fix',
    [string]$ClangToolchain = 'P:\BC-250\toolchain\llvm-23.1.4-win64',
    [int]$Jobs = 8,
    [string[]]$Tests = @('radv_wddm2_ib2_gather_test', 'radv_wddm2_deferred_destroy_test'),
    [string]$Runner = 'P:\BC-250\scratch\bd201-fix\tools\run-winsys-tests.ps1',
    # Macros for the test sources only: IB2_GATHER_NEGATIVE_CONTROL for a tree before the change.
    [string[]]$Define = @()
)
$ErrorActionPreference = 'Stop'
$env:BC250_ROOT = 'P:\BC-250'
$env:PYTHONDONTWRITEBYTECODE = '1'
$shell = (Get-Process -Id $PID).Path
$run = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$head = (& git -C $Source rev-parse HEAD).Trim()
"$([DateTime]::UtcNow.ToString('o')) source $Source at $head, recipe $((& git -C $Recipe rev-parse HEAD).Trim())"
& git -C $Source status --short | ForEach-Object { "  $_" }
$failed = 0
foreach ($flavour in $Flavours) {
    $name = "b-system-$Tag-$flavour"
    $out = Join-Path $OutRoot $name
    if (Test-Path -LiteralPath $out) { throw "$out exists: use a new -Tag" }
    $tmp = Join-Path $OutRoot "tmp-$name"; New-Item -ItemType Directory -Force $tmp | Out-Null
    $a = @('-Config', 'radv-system', '-Source', $Source, '-Build', $out, '-Arch', 'x64', '-Jobs', $Jobs,
           '-Compiler', 'clang-cl', '-Flavour', $flavour, '-Temp', $tmp, '-ClangToolchain', $ClangToolchain,
           '-Variant', 'release,isa')
    if ($flavour -eq 'lab') { $a += '-Lab' }
    $log = Join-Path $OutRoot "build-$name.log"
    "$([DateTime]::UtcNow.ToString('o')) build $name"
    & $shell -NoProfile -File (Join-Path $Recipe 'tools\build\build-mesa.ps1') @a *> $log
    if ($LASTEXITCODE -ne 0) { "build $name exit $LASTEXITCODE (see $log)"; $failed++; continue }
    foreach ($dll in @(Get-ChildItem -LiteralPath (Join-Path $out 'src\amd\vulkan') -Filter '*.dll' -File)) {
        "$([DateTime]::UtcNow.ToString('o')) $name $($dll.Name) $((Get-FileHash -LiteralPath $dll.FullName).Hash) $($dll.Length) bytes"
    }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($t in $Tests) {
        if (-not (Test-Path -LiteralPath (Join-Path $Source "src\amd\vulkan\winsys\wddm2\tests\$t.c"))) { "no $t in the tree"; continue }
        $o = Join-Path $OutRoot "t-$Tag-$flavour-$t-$run"
        $d = if ($Define.Count) { @('-Define', ($Define -join ',')) } else { @() }
        & $shell -NoProfile -File $Runner -Source $Source -Build $out -OutputDir $o -Test "src/amd/vulkan/winsys/wddm2/tests/$t.c" @d *> "$o.log"
        $code = $LASTEXITCODE
        $cases = @(Select-String -LiteralPath "$o.log" -Pattern 'checks=(\d+), failures=(\d+)')
        $sum = { param($i) ($cases | ForEach-Object { [int]$_.Matches[0].Groups[$i].Value } | Measure-Object -Sum).Sum }
        "$([DateTime]::UtcNow.ToString('o')) $t $flavour exit ${code}: $($cases.Count) cases, $(& $sum 1) checks, $(& $sum 2) failures"
        Get-Content -LiteralPath "$o.log" | Where-Object { $_ -match '^\s+FAIL|exit=' } | ForEach-Object { "  $_" }
        if ($code -ne 0) { $failed++ }
        Remove-Item -LiteralPath "$o-tmp" -Recurse -Force -ErrorAction SilentlyContinue
    }
}
"$([DateTime]::UtcNow.ToString('o')) done, $failed failed step(s)"
exit [int]($failed -ne 0)
