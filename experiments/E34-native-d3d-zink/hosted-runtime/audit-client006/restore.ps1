. "$PSScriptRoot\common.ps1"
$mutex=New-Object Threading.Mutex($false,'Global\BC250AuditClient006Restore')
$locked=$false
try {
 try{$locked=$mutex.WaitOne(15000)}catch [Threading.AbandonedMutexException]{$locked=$true}
 if(!$locked){throw 'Restore mutex timeout'}
 if(Test-Path -LiteralPath "$d\enable"){Remove-Item -LiteralPath "$d\enable" -Force}
 $errors=New-Object 'System.Collections.Generic.List[string]'
 foreach($child in @(Get-Control)){try{$child.Kill();if(!$child.WaitForExit(5000)){throw 'Audit child remains live'}}catch{$errors.Add($_.Exception.Message)}}
 foreach($item in $items){
  try{Restore-DurableBaseline $item.path $item.backup $item.original $item.baseline $item.candidate}catch{$errors.Add($_.Exception.Message)}
 }
 if($errors.Count){throw ($errors -join '; ')}
 if(!(Test-Path -LiteralPath "$d\restored.json")){
  Write-DurableText "$d\restored.json" (@{utc=[DateTime]::UtcNow.ToString('o');umd=(Get-FileHash $items[0].path).Hash;icd=(Get-FileHash $items[1].path).Hash}|ConvertTo-Json)
 }
}finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
