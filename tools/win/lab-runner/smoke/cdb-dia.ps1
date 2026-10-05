# LAB (elevated SSH): put msdia140.dll (pushed to C:\BC250\tmp\cdb-dia) next to the lab's portable cdb, so dbghelp
# can open our PDBs (without it every PDB load fails with 0x8007007E and stacks show export names only).
$src = 'C:\BC250\tmp\cdb-dia\msdia140.dll'
$dst = 'C:\BC250\tools\cdb\msdia140.dll'
if (-not (Test-Path -LiteralPath $src)) { "missing $src"; exit 2 }
if (Test-Path -LiteralPath $dst) { 'already present: ' + (Get-FileHash -LiteralPath $dst).Hash; exit 0 }
Copy-Item -LiteralPath $src -Destination $dst
'copied: ' + (Get-FileHash -LiteralPath $dst).Hash
