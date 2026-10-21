# Offline test of installer\mesa-temp-cleanup.ps1 (BD-211): which files the installer takes for leftovers of Mesa's
# u_memstream and removes, and which it leaves. Works only in a folder under -Out; reads no real temp folder.
#
#   powershell -NoProfile -File tools\release\test-mesa-temp-cleanup-installer.ps1 -Out <folder>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer\mesa-temp-cleanup.ps1')
$script:checks = 0
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw "FAIL: $Message" }; $script:checks++ }

$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('mesa-temp-' + [Guid]::NewGuid().ToString('N'))
$a = Join-Path $base 'profile-a'; $b = Join-Path $base 'windows-temp'; $missing = Join-Path $base 'no-such-folder'
[void][IO.Directory]::CreateDirectory($a); [void][IO.Directory]::CreateDirectory($b)
$now = [DateTime]::UtcNow
function New-TestFile([string]$Dir, [string]$Name, [int]$AgeHours, [int]$Bytes = 100) {
    $p = Join-Path $Dir $Name
    [IO.File]::WriteAllBytes($p, (New-Object byte[] $Bytes))
    [IO.File]::SetLastWriteTimeUtc($p, $now.AddHours(-$AgeHours))
    return $p
}
# Leftovers: the GetTempFileName names of the prefix "MEMSTREAM", older than a day, in either case.
$old = @(
    (New-TestFile $a 'MEM1.TMP' 48), (New-TestFile $a 'MEMFFFF.tmp' 30 2048), (New-TestFile $a 'mem3a2b.tmp' 25),
    (New-TestFile $b 'MEM0A1.TMP' 200 512))
# Not leftovers: too new, or another name.
$keep = @(
    (New-TestFile $a 'MEM2.TMP' 2), (New-TestFile $a 'MEM12345.TMP' 48), (New-TestFile $a 'MEMG1.TMP' 48),
    (New-TestFile $a 'MEMSTREAM.TMP' 48), (New-TestFile $a 'XMEM1.TMP' 48), (New-TestFile $a 'MEM1.TMP.bak' 48),
    (New-TestFile $a 'MEM.TMP' 48), (New-TestFile $b 'MEM1A.log' 48))
# An old leftover that is still open without delete sharing, as a running program keeps it.
$open = New-TestFile $b 'MEMBEEF.TMP' 48
# An old leftover marked read-only: the delete is refused.
$ro = New-TestFile $b 'MEMC0DE.TMP' 48; (Get-Item -LiteralPath $ro).IsReadOnly = $true

$found = Find-MesaTempLeftovers -Roots @($a, $b, $missing) -NowUtc $now
$names = @($found.files | ForEach-Object Name | Sort-Object)
Assert ($found.count -eq 6) "found $($found.count) leftovers, expected 6: $($names -join ', ')"
foreach ($p in $old + @($open, $ro)) { Assert ($names -contains [IO.Path]::GetFileName($p)) "missed $p" }
foreach ($p in $keep) { Assert (-not ($names -contains [IO.Path]::GetFileName($p))) "took $p" }
Assert ($found.young -eq 1) "young $($found.young), expected 1 (MEM2.TMP)"
Assert ($found.unreadable -eq 1) "unreadable $($found.unreadable), expected 1 (the missing folder)"
Assert ($found.bytes -eq (100 + 2048 + 100 + 512 + 100 + 100)) "bytes $($found.bytes)"
# The age bound is a parameter: with no minimum age the new file counts too.
Assert ((Find-MesaTempLeftovers -Roots @($a) -NowUtc $now -MinAge ([TimeSpan]::Zero)).count -eq 4) 'MinAge 0 did not take the new file'

$fs = [IO.File]::Open($open, 'Open', 'ReadWrite', 'None')
try { $done = Remove-MesaTempLeftovers -Files $found.files } finally { $fs.Dispose() }
Assert ($done.removed -eq 4) "removed $($done.removed), expected 4"
Assert ($done.bytes -eq (100 + 2048 + 100 + 512)) "removed bytes $($done.bytes)"
Assert ($done.in_use -eq 1) "in use $($done.in_use), expected 1"
Assert ($done.denied -eq 1) "denied $($done.denied), expected 1"
foreach ($p in $old) { Assert (-not (Test-Path -LiteralPath $p)) "left $p" }
foreach ($p in $keep + @($open, $ro)) { Assert (Test-Path -LiteralPath $p) "removed $p" }
# A second pass over the same list (the program has closed its file since): the closed file goes, the read-only one
# stays refused, and the files already gone count as nothing and do not fail.
$again = Remove-MesaTempLeftovers -Files $found.files
Assert ($again.removed -eq 1 -and $again.in_use -eq 0 -and $again.denied -eq 1) "second pass: removed $($again.removed), in use $($again.in_use), denied $($again.denied)"
Assert (-not (Test-Path -LiteralPath $open)) 'second pass left the closed file'

(Get-Item -LiteralPath $ro).IsReadOnly = $false
Remove-Item -LiteralPath $base -Recurse -Force
"mesa-temp-cleanup: $($script:checks) checks passed"
