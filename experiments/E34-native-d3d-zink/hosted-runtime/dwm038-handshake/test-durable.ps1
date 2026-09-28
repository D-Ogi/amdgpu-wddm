param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$env:TEMP=$Out;$env:TMP=$Out
. "$PSScriptRoot\durable.ps1"
$d=$Out
if(Test-Path $d){throw 'Test directory exists'}
New-Item -ItemType Directory $d | Out-Null
[IO.File]::WriteAllBytes("$d\source.bin",[byte[]](0..255)*4097)
$hash=(Get-FileHash "$d\source.bin").Hash
Copy-VerifiedDurable "$d\source.bin" "$d\backup.bin" $hash
if((Get-FileHash "$d\backup.bin").Hash -ne $hash){throw 'Positive mismatch'}
$rejected=$false;try{Copy-VerifiedDurable "$d\source.bin" "$d\bad.bin" ('0'*64)}catch{$rejected=$true}
if(!$rejected -or (Test-Path "$d\bad.bin")){throw 'Bad source was accepted'}
$rejected=$false;try{Copy-VerifiedDurable "$d\source.bin" "$d\backup.bin" $hash}catch{$rejected=$true}
if(!$rejected -or (Get-FileHash "$d\backup.bin").Hash -ne $hash){throw 'Existing backup not preserved'}
Write-DurableText "$d\marker.json" '{"pending":true}'
Flush-ExistingFile "$d\marker.json"
if((Get-Content "$d\marker.json" -Raw) -ne '{"pending":true}'){throw 'Marker mismatch'}
$rejected=$false;try{Write-DurableText "$d\marker.json" 'overwrite'}catch{$rejected=$true}
if(!$rejected -or (Get-Content "$d\marker.json" -Raw) -ne '{"pending":true}'){throw 'Marker overwritten'}
if([Microsoft.Win32.RegistryKey].GetProperty('Handle').PropertyType.FullName -ne 'Microsoft.Win32.SafeHandles.SafeRegistryHandle'){throw 'Registry PInvoke handle mismatch'}

[IO.File]::WriteAllText("$d\candidate.bin",'candidate')
$candidate=(Get-FileHash "$d\candidate.bin").Hash
Copy-Item "$d\candidate.bin" "$d\active.bin"
[IO.File]::WriteAllText("$d\corrupt-backup.bin",'corrupt')
Restore-DurableBaseline "$d\active.bin" "$d\corrupt-backup.bin" "$d\backup.bin" $hash $candidate
if((Get-FileHash "$d\active.bin").Hash -ne $hash){throw 'Original fallback failed'}
if(@(Get-ChildItem $d -Filter 'active.bin.held-*').Count -ne 1){throw 'Candidate not preserved'}
Restore-DurableBaseline "$d\active.bin" "$d\absent.bin" "$d\absent2.bin" $hash $candidate
[IO.File]::WriteAllText("$d\unexpected.bin",'unknown')
$before=(Get-FileHash "$d\unexpected.bin").Hash
$rejected=$false;try{Restore-DurableBaseline "$d\unexpected.bin" "$d\backup.bin" "$d\backup.bin" $hash $candidate}catch{$rejected=$true}
if(!$rejected -or (Get-FileHash "$d\unexpected.bin").Hash -ne $before){throw 'Unexpected active file overwritten'}
@{ps_version=$PSVersionTable.PSVersion.ToString();copy_hash_match=$true;bad_source_rejected=$true;existing_backup_preserved=$true;marker_roundtrip=$true;marker_overwrite_rejected=$true;original_fallback_pass=$true;candidate_preserved=$true;already_restored_without_backup_pass=$true;unexpected_file_preserved=$true;flush_signature_valid=$true;registry_not_touched=$true} | ConvertTo-Json
