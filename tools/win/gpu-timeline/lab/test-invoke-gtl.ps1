# Host check of gtl-run.ps1's Invoke-Gtl under Windows PowerShell 5.1 (the lab's shell), without the device:
# the function is taken from gtl-run.ps1's own syntax tree, then runs gpu-timeline.exe's selftest (exit 0), a
# refused bound (exit 2) and a probe without bc250rd (exit 1), and a kill of an overlong child.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\gpu-timeline\lab\test-invoke-gtl.ps1
#
# -Build names the directory that holds gpu-timeline.exe; build.ps1 passes its own -Out through BC250_GTL_BUILD,
# because the build artifacts stay outside this repository.
param([string]$Build = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Build) {
    $Build = if ($env:BC250_GTL_BUILD) { $env:BC250_GTL_BUILD } else {
        $repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
        $root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Split-Path -Parent $repo) }
        Join-Path $root 'scratch\build\gpu-timeline'
    }
}
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $here 'gtl-run.ps1'), [ref]$null, [ref]$null)
$fn = $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-Gtl' }, $true)
if ($fn.Count -ne 1) { throw 'Invoke-Gtl not found in gtl-run.ps1' }
. ([ScriptBlock]::Create($fn[0].Extent.Text))
$Exe = Join-Path $Build 'gpu-timeline.exe'
$tmp = Join-Path $Build 'invoke-test'
$null = New-Item -ItemType Directory -Force -Path $tmp
$fail = 0
function Check($what, $got, $want) {
    if ($got -eq $want) { "ok   $what ($got)" } else { "FAIL ${what}: got $got, want $want"; $script:fail++ }
}
Check 'selftest exit' (Invoke-Gtl "selftest --out `"$tmp\s.gtl`"" "$tmp\selftest" 30) 0
Check 'selftest stdout' ([bool](Select-String -LiteralPath "$tmp\selftest.txt" -SimpleMatch -Pattern 'CP only 20.0 %')) $true
Check 'bound exit' (Invoke-Gtl 'sample --seconds 61 --out x' "$tmp\bound" 30) 2
Check 'bound stderr' ([bool](Select-String -LiteralPath "$tmp\bound-err.txt" -SimpleMatch -Pattern '--seconds must be 1..60')) $true
Check 'probe without driver exit' (Invoke-Gtl 'probe' "$tmp\probe" 30) 1
# A child that outlives its limit is killed and reported as -1 (ping waits about 1 s per echo).
$Exe = "$env:SystemRoot\System32\PING.EXE"
$t0 = Get-Date
Check 'kill after limit' (Invoke-Gtl '-n 30 127.0.0.1' "$tmp\kill" 2) -1
# 10 s, not 5: the kill itself is immediate, but on a loaded development PC the start of the child and the two
# text files cost seconds. A 5 s bound failed once right after the compile and the analyzer's tests.
Check 'killed within 10 s' (((Get-Date) - $t0).TotalSeconds -lt 10) $true
Remove-Item -Recurse -Force -LiteralPath $tmp
if ($fail) { exit 1 }
