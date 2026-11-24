# Host test of stick-env.ps1. No stick, no lab. Usage: stick-env.test.ps1 -Work <empty directory for test files>
# It writes blocks into -Work so that a caller can compare them with `grub-editenv <file> list` (see README).
param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\stick-env.ps1"
$script:fail = 0
function Check([string]$What, [bool]$Ok) {
    if ($Ok) { "  PASS $What" } else { "  FAIL $What"; $script:fail++ }
}
function Refuses([string]$What, [scriptblock]$Do) {
    $threw = $false
    try { & $Do | Out-Null } catch { $threw = $true }
    Check "refused: $What" $threw
}

$empty = New-StickEnv
Check 'a new block is 1024 bytes' ($empty.Length -eq 1024)
Check 'a new block has no variable' ((Read-StickEnv $empty).Count -eq 0)

$b = Set-StickEnvValue $empty 'bc250_default' '4'
$b = Set-StickEnvValue $b 'bc250_stage_set' 'tpm'
$b = Set-StickEnvValue $b 'bc250_stage_left' '2'
$t = Read-StickEnv $b
Check 'three values read back' ($t['bc250_default'] -eq '4' -and $t['bc250_stage_set'] -eq 'tpm' -and $t['bc250_stage_left'] -eq '2')
Check 'the block stays 1024 bytes' ($b.Length -eq 1024)

# A block as GRUB leaves it: the counter and a foreign line. Both must survive a change of another name.
$grub = Format-StickEnv ([ordered]@{ bc250_try = '2'; saved_entry = 'x' })
$g2 = Set-StickEnvValue $grub 'bc250_default' '0'
$t = Read-StickEnv $g2
Check 'the counter survives a change of another name' ($t['bc250_try'] -eq '2')
Check 'a foreign line survives' ($t['saved_entry'] -eq 'x')
$g3 = Set-StickEnvValue $g2 'bc250_default' ''
Check 'an empty value removes the name' (-not (Read-StickEnv $g3).Contains('bc250_default'))
Check 'removal gives the first block back, byte for byte' ([Convert]::ToBase64String($g3) -eq [Convert]::ToBase64String($grub))
$g4 = Set-StickEnvValue $grub 'bc250_try' ''
Check 'the counter can be cleared' (-not (Read-StickEnv $g4).Contains('bc250_try'))

Refuses 'a name that the menu does not read' { Set-StickEnvValue $empty 'bc250_stage_args' 'sb=0' }
Refuses 'option text in place of a set name' { Set-StickEnvValue $empty 'bc250_stage_set' 'unlock=1 sb=0' }
Refuses 'a default outside 0 to 5' { Set-StickEnvValue $empty 'bc250_default' '8' }
Refuses 'a counter value from a tool' { Set-StickEnvValue $empty 'bc250_try' '1' }
Refuses 'a value with a line break' { Set-StickEnvValue $empty 'bc250_stage_always' "tpm`nbc250_default=0" }
Refuses 'a block of the wrong size' { Read-StickEnv ([byte[]]::new(1023)) }
$bad = [byte[]]$empty.Clone(); $bad[0] = 0x58
Refuses 'a block with a wrong header' { Read-StickEnv $bad }
$dup = [Text.Encoding]::ASCII.GetBytes("# GRUB Environment Block`nbc250_try=1`nbc250_try=2`n")
$dup = $dup + [byte[]](,35 * (1024 - $dup.Length))
Refuses 'a block with a name twice' { Read-StickEnv $dup }

[IO.File]::WriteAllBytes((Join-Path $Work 'grubenv-three'), $b)
[IO.File]::WriteAllBytes((Join-Path $Work 'grubenv-empty'), $empty)
if ($script:fail) { "STICK-ENV TEST: $($script:fail) FAIL"; exit 1 }
'STICK-ENV TEST: ALL PASS'
