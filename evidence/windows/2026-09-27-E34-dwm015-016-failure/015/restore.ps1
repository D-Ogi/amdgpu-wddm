param([switch]$Restart)
$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted015'
$mutex=New-Object Threading.Mutex($false,'Global\BC250G0DwmRestore014')
$locked=$false
try {
 try {$locked=$mutex.WaitOne(15000)} catch [Threading.AbandonedMutexException] {$locked=$true}
 if(!$locked){throw 'Restore mutex timeout'}
 Remove-Item -LiteralPath "$d\enable" -Force -ErrorAction SilentlyContinue
 $cfg=Get-Content "$d\manifest.json" -Raw | ConvertFrom-Json
 $errors=New-Object 'System.Collections.Generic.List[string]'
 foreach($item in @(@{path='C:\BC250\m11\resource-close\bc250d3d.dll';backup="$d\baseline-umd.dll";baseline='8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA';candidate=$cfg.'router.dll'},@{path='C:\BC250\m10\wsi-final\vulkan_radeon.dll';backup="$d\baseline-icd.dll";baseline='93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D';candidate='7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'})) {
  try {
   if((Get-FileHash $item.backup).Hash -ne $item.baseline){throw 'Backup mismatch'}
   $current=if(Test-Path -LiteralPath $item.path){(Get-FileHash $item.path).Hash}else{''}
   if($current -eq $item.candidate){Move-Item -LiteralPath $item.path -Destination ($item.path+'.dwm015-held-'+[guid]::NewGuid().ToString('N'));$current=''}
   if(!$current){Copy-Item -LiteralPath $item.backup -Destination $item.path}
   if((Get-FileHash $item.path).Hash -ne $item.baseline){throw 'Unexpected active file; not overwritten'}
  } catch {$errors.Add($_.Exception.Message)}
 }
 if($errors.Count){throw ($errors -join '; ')}
 if($Restart -and !(Test-Path "$d\restored.json")) {
  $ids=@(Get-Process dwm | Select-Object -ExpandProperty Id)
  foreach($idValue in $ids){Stop-Process -Id $idValue -Force}
  Start-Sleep -Seconds 3
  @{utc=[DateTime]::UtcNow.ToString('o');before=$ids;after=@(Get-Process dwm | Select-Object -ExpandProperty Id)} | ConvertTo-Json | Set-Content "$d\restored.json"
 }
 'Baselines verified'
} finally {if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
