param([Parameter(Mandatory)][string]$Out,[Parameter(Mandatory)][string]$Durable)
$ErrorActionPreference='Stop'
. $Durable
. "$PSScriptRoot\file-routing.ps1"
if(Test-Path $Out){throw 'Fresh directory required'}
New-Item -ItemType Directory $Out|Out-Null
function Reject([scriptblock]$Action){$failed=$false;try{& $Action}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
$active="$Out\active.dll";$backup="$Out\backup.dll";$original="$Out\original.dll";$source="$Out\router.dll"
[IO.File]::WriteAllText($active,'baseline');[IO.File]::WriteAllText($backup,'baseline');[IO.File]::WriteAllText($source,'candidate')
$baseline=(Get-FileHash $active).Hash;$candidate=(Get-FileHash $source).Hash
Install-M14FileRoute $active $backup $original $source $baseline $candidate
if((Get-FileHash $active).Hash -ne $candidate -or (Get-FileHash $original).Hash -ne $baseline){throw 'Install mismatch'}
Restore-M14FileRoute $active $backup $baseline $candidate
Restore-M14FileRoute $active $backup $baseline $candidate
if((Get-FileHash $active).Hash -ne $baseline){throw 'Restore mismatch'}
Reject {Install-M14FileRoute $active $backup $original $source $baseline $candidate}
# Interrupted after the original rename, before candidate placement.
Move-Item -LiteralPath $active -Destination "$Out\interrupted-original.dll"
Restore-M14FileRoute $active $backup $baseline $candidate
if((Get-FileHash $active).Hash -ne $baseline){throw 'Missing-active recovery failed'}
[IO.File]::WriteAllText($active,'foreign')
Reject {Restore-M14FileRoute $active $backup $baseline $candidate}
if([IO.File]::ReadAllText($active) -ne 'foreign'){throw 'Clobbered foreign file'}
[IO.File]::WriteAllText($active,'baseline');[IO.File]::WriteAllText($source,'bad candidate')
Reject {Install-M14FileRoute $active $backup "$Out\second-original.dll" $source $baseline $candidate}
if((Get-FileHash $active).Hash -ne $baseline -or (Test-Path "$Out\second-original.dll")){throw 'Mutation before source admission'}
'PASS file routing: exact install/restore, idempotence, interrupted rename, foreign state and source hash refusal'
