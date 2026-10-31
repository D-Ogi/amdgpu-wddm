#Requires -Version 7.0
# Host test of PackageRound.psm1 (BD-272): every output of an earlier package round is named in one message before
# the first step, and -SetAside moves them into a round directory with the same SHA-256 for every file.
#   pwsh -File tools\release\test-package-round.ps1 -Out <dir>      (the 'package-round' gate of quick.ps1)
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PackageRound.psm1') -Force
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('package-round-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$from = Join-Path $base 'set'
$version = '0.7.216.999-tester.99'
$passed = 0
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }
function New-File([string]$Path, [string]$Text) { New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null; [IO.File]::WriteAllText($Path, $Text) }

New-Item -ItemType Directory -Force $from | Out-Null
Assert (@(Get-PackageRoundOutputs $from @('release', 'lab') $version).Count -eq 0) 'a new set has no earlier outputs'

# The outputs of the r6 round of tester.34: the setup windows, the release package and the CAB.
New-File (Join-Path $from 'setup\release\amdgpu_wddm_setup.exe') 'setup release'
New-File (Join-Path $from 'setup\lab\amdgpu_wddm_setup.exe') 'setup lab'
New-File (Join-Path $from 'package\release\pkg\files\RELEASE-NOTES.md') 'notes'
New-File (Join-Path $from 'package\release\pkg.zip') 'zip'
New-File (Join-Path $from "submission\amdgpu-wddm-$version.cab") 'cab'
New-File (Join-Path $from 'submission\amdgpu-wddm-0.7.216.1-tester.1.cab') 'the CAB of another version'
New-Item -ItemType Directory -Force (Join-Path $from 'package-wt-lab') | Out-Null
$found = @(Get-PackageRoundOutputs $from @('release', 'lab') $version)
Assert ($found.Count -eq 4) "four outputs, not $($found.Count): $(($found | ForEach-Object { $_.path }) -join ', ')"
Assert (@($found | Where-Object { $_.kind -eq 'submission CAB' }).Count -eq 1) 'the CAB of this version only'
$message = Format-PackageRoundOutputs $found
foreach ($p in 'setup\release', 'setup\lab', 'package\release', "amdgpu-wddm-$version.cab") {
    Assert ($message.Contains($p)) "the one message names $p"
}
$parallel = @(Get-PackageRoundOutputs $from @('release', 'lab') $version -Parallel)
Assert ($parallel.Count -eq 5 -and @($parallel | Where-Object { $_.worktree }).Count -eq 1) 'with -Parallel, the worktree of a flavour too'
Assert ((Format-PackageRoundOutputs $parallel).Contains('git worktree remove')) 'a worktree is removed with git'
Assert (@(Get-PackageRoundOutputs $from @('lab') $version).Count -eq 1) 'a lab round refuses only the lab outputs'

# -SetAside refuses a worktree and a bad name before it moves anything.
$refused = $false
try { Move-PackageRoundOutputs $from $parallel 'r6' } catch { $refused = $_.Exception.Message -match 'worktree' }
Assert ($refused -and (Test-Path (Join-Path $from 'setup\release'))) 'a worktree output stops -SetAside, and nothing moves'
$refused = $false
try { Move-PackageRoundOutputs $from $found '..\x' } catch { $refused = $true }
Assert $refused 'a round name with a path is refused'

# -SetAside moves the four outputs with their hashes.
$before = Get-OutputHashes (Join-Path $from 'package\release')
$record = Move-PackageRoundOutputs $from $found 'r6'
$round = Join-Path $from 'rounds\r6'
Assert (@(Get-PackageRoundOutputs $from @('release', 'lab') $version).Count -eq 0) 'nothing to refuse after the move'
$after = Get-OutputHashes (Join-Path $round 'package\release')
Assert (($before | ConvertTo-Json) -eq ($after | ConvertTo-Json)) 'the package keeps every file and hash'
Assert (Test-Path (Join-Path $round "submission\amdgpu-wddm-$version.cab")) 'the CAB is in the round'
Assert (Test-Path (Join-Path $from 'submission\amdgpu-wddm-0.7.216.1-tester.1.cab')) 'the CAB of another version stays'
$json = @(Get-Content -LiteralPath (Join-Path $round 'set-aside.json') -Raw | ConvertFrom-Json -AsHashtable)
Assert ($json.Count -eq 1 -and $json[0].outputs.Count -eq 4) 'set-aside.json records the four outputs'
$pkg = @($json[0].outputs | Where-Object { $_.kind -eq 'package' })[0]
Assert ($pkg.sha256['pkg/files/RELEASE-NOTES.md'] -eq (Get-FileHash -LiteralPath (Join-Path $round 'package\release\pkg\files\RELEASE-NOTES.md')).Hash) 'the record holds the hash of each file'

# A second round with the same name does not overwrite the first.
New-File (Join-Path $from 'setup\release\amdgpu_wddm_setup.exe') 'setup release r7'
$refused = $false
try { Move-PackageRoundOutputs $from @(Get-PackageRoundOutputs $from @('release') $version) 'r6' } catch { $refused = $_.Exception.Message -match 'already holds' }
Assert ($refused -and (Get-Content -Raw (Join-Path $round 'setup\release\amdgpu_wddm_setup.exe')) -eq 'setup release') 'a round keeps its outputs'
$null = Move-PackageRoundOutputs $from @(Get-PackageRoundOutputs $from @('release') $version) 'r7'
Assert (Test-Path (Join-Path $from 'rounds\r7\setup\release\amdgpu_wddm_setup.exe')) 'another round name takes it'

# build-release-set.ps1 checks the outputs before the work ledger gate and before the first step of a flavour.
$text = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build-release-set.ps1') -Raw
$at = $text.IndexOf('Get-PackageRoundOutputs $From')
Assert ($at -gt 0) 'build-release-set.ps1 calls Get-PackageRoundOutputs'
foreach ($later in "scratch\ledger\ledger.py", 'check_source_versions.py', 'amdgpu_wddm_setup\build.ps1', 'git -C $repo worktree add') {
    $i = $text.LastIndexOf($later)
    Assert ($i -gt $at) "the check comes before $later"
}

Remove-Item -LiteralPath $base -Recurse -Force
"PASS: package round outputs, $passed checks (BD-272)"
