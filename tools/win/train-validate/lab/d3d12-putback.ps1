#Requires -Version 5
# The D3D12 putback of a promotion. A release whose D3D12 triplet differs from the accepted one installs its own
# files in <InstallRoot>\d3d12, but the promotion attempt's Capture verifies the ACCEPTED triplet there before it
# swaps the candidate in (release-baseline.py --keep-accepted-d3d12: "the release went in first and the operator put
# the accepted triplet back"). So, before the stage:
#
#   putback   every staged accepted file (-Source, one file per triplet name) is checked against -Expect, the live
#             release file is kept next to it as <name>.release, and the accepted file goes in its place. A live
#             file that is already the accepted one is left alone, and so is its .release copy: a resumed run
#             must not overwrite the release file with the accepted one.
#   clean     after the promotion: a <name>.release whose bytes are the live file again (the attempt retained the
#             release triplet) is deleted. Any other .release stays, and the line says why.
#
# -Expect 'amdgpu_wddm_d3d12.dll=<sha256>;...' names the accepted hashes (lab-baseline.json d3d12.accepted). It
# names no train and no package. Result line: 'd3d12 putback OK' or 'd3d12 putback FAILED <why>' (and the same
# with 'clean').
param(
    [Parameter(Mandatory)][ValidateSet('putback', 'clean')][string]$Step,
    [string]$Source = '',
    [string]$Expect = ''
)
$ErrorActionPreference = 'Stop'
$names = 'amdgpu_wddm_d3d12.dll', 'amdgpu_wddm_vkd3d.dll', 'amdgpu_wddm_radv.dll'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$dir = Join-Path $inst 'd3d12'
function Hash([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function Show([string]$tag) {
    "--- $tag"
    Get-ChildItem -LiteralPath $dir -File -Force | Sort-Object Name | ForEach-Object { '  {0} {1,10} {2}' -f (Hash $_.FullName), $_.Length, $_.Name }
}
Show 'before'
$failed = @()
if ($Step -eq 'putback') {
    $want = @{}
    foreach ($pair in @($Expect -split ';' | Where-Object { $_ })) {
        $name, $sha = $pair -split '=', 2
        if ($names -notcontains $name -or $sha -notmatch '^[0-9A-Fa-f]{64}$') { throw "malformed -Expect entry '$pair'" }
        $want[$name] = $sha.ToUpperInvariant()
    }
    if (-not $want.Count) { throw '-Expect names no file' }
    foreach ($name in $want.Keys) {
        $src = Join-Path $Source $name
        $live = Join-Path $dir $name
        $kept = "$live.release"
        if (-not (Test-Path -LiteralPath $src)) { $failed += "$name has no staged copy at $src"; continue }
        if ((Hash $src) -ne $want[$name]) { $failed += "$name staged copy is $((Hash $src).Substring(0, 8)), not the accepted $($want[$name].Substring(0, 8))"; continue }
        if ((Test-Path -LiteralPath $live) -and (Hash $live) -eq $want[$name]) { "$name is the accepted file already"; continue }
        if (Test-Path -LiteralPath $live) { Copy-Item -LiteralPath $live -Destination $kept -Force; "$name kept the release file as $(Split-Path -Leaf $kept)" }
        Copy-Item -LiteralPath $src -Destination $live -Force
        $now = Hash $live
        if ($now -ne $want[$name]) { $failed += "$name is $($now.Substring(0, 8)) after the copy" } else { "$name is the accepted $($now.Substring(0, 8)) now" }
    }
} else {
    foreach ($name in $names) {
        $live = Join-Path $dir $name
        $kept = "$live.release"
        if (-not (Test-Path -LiteralPath $kept)) { continue }
        if ((Test-Path -LiteralPath $live) -and (Hash $live) -eq (Hash $kept)) {
            Remove-Item -LiteralPath $kept -Force
            "$name.release deleted: the live file is the release file again"
        } else {
            $failed += "$name.release stays: the live file is not the release file, so the promotion did not retain it"
        }
    }
}
Show 'after'
$failed | ForEach-Object { "  $_" }
if ($failed) { "d3d12 $Step FAILED $($failed[0])"; exit 1 }
"d3d12 $Step OK"
exit 0
