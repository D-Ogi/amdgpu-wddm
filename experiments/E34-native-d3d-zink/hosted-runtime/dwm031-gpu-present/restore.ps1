param([switch]$Restart)
$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted031'
. "$PSScriptRoot\durable.ps1"
$mutex=New-Object Threading.Mutex($false,'Global\BC250G0DwmRestore014')
$locked=$false
try {
 try {$locked=$mutex.WaitOne(15000)} catch [Threading.AbandonedMutexException] {$locked=$true}
 if(!$locked){throw 'Restore mutex timeout'}
 Remove-Item -LiteralPath "$d\enable" -Force -ErrorAction SilentlyContinue
 $errors=New-Object 'System.Collections.Generic.List[string]'
 # Attempt both gate closure and DLL restoration even if either reports failure.
 try {Set-DurablePresentGates 0} catch {$errors.Add($_.Exception.Message)}
 $cfg=Get-Content "$d\manifest.json" -Raw | ConvertFrom-Json
 foreach($item in @(@{path='C:\BC250\m11\resource-close\bc250d3d.dll';backup="$d\baseline-umd.dll";baseline='8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA';candidate=$cfg.'router.dll'},@{path='C:\BC250\m10\wsi-final\vulkan_radeon.dll';backup="$d\baseline-icd.dll";baseline='CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157';candidate='7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'})) {
  try {
   Restore-DurableBaseline $item.path $item.backup ($item.backup.Replace('baseline-','original-')) $item.baseline $item.candidate
  } catch {$errors.Add($_.Exception.Message)}
 }
 try {& "$d\interop.ps1" -Value 0} catch {$errors.Add($_.Exception.Message)}
 if($errors.Count){throw ($errors -join '; ')}
 if($Restart -and !(Test-Path "$d\restored.json")) {
  $ids=@(Get-Process dwm | Select-Object -ExpandProperty Id)
  foreach($idValue in $ids){Stop-Process -Id $idValue -Force}
  Start-Sleep -Seconds 3
  @{utc=[DateTime]::UtcNow.ToString('o');before=$ids;after=@(Get-Process dwm | Select-Object -ExpandProperty Id)} | ConvertTo-Json | Set-Content "$d\restored.json"
 }
 'Baselines verified'
} finally {if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
