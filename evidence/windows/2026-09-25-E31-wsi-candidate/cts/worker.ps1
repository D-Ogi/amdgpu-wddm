$ErrorActionPreference='Stop'
$out='C:\BC250\m10\cts-smoke-04'
$env:VK_DRIVER_FILES='C:\BC250\m10\wsi-fifo\radeon_icd.json'
$env:VK_ICD_FILENAMES=$env:VK_DRIVER_FILES
$env:MESA_VK_WSI_DEBUG=''
$env:BC250_TRACE_SUBMITS='0'
$env:VK_LOADER_DEBUG='driver'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$exe="$out\deqp-vk.exe"
try {
 foreach($case in (Get-Content "$out\cases.txt")){
  if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
  $stem=$case.Replace('dEQP-VK.wsi.win32.','')
  $args=@("--deqp-case=$case","--deqp-log-filename=$out\$stem.qpa",'--deqp-watchdog=enable','--deqp-watchdog-total-time-limit=35','--deqp-watchdog-interval-time-limit=20','--deqp-log-images=enable','--deqp-log-shader-sources=enable')
  Add-Content "$out\progress.txt" ("START "+$case+" "+(Get-Date).ToString('o'))
  $p=Start-Process -FilePath $exe -WorkingDirectory $out -ArgumentList $args -PassThru -WindowStyle Normal -RedirectStandardOutput "$out\$stem.out" -RedirectStandardError "$out\$stem.err"
  $keepHandle=$p.Handle
  if(-not $p.WaitForExit(45000)){Stop-Process -Id $p.Id -Force;throw "TIMEOUT $case"}
  $driverLog=Get-Content "$out\$stem.err" -Raw
  if($driverLog -notmatch 'C:\\BC250\\m10\\wsi-fifo\\(?:\.\\)?vulkan_radeon.dll' -or $driverLog -match 'C:\\BC250\\m8\\vulkan_radeon.dll'){throw "Wrong or missing ICD witness $case"}
  $content=Get-Content "$out\$stem.qpa" -Raw
  if($p.ExitCode -ne 0 -or $content -notmatch '<Result StatusCode="Pass">'){
   throw "FAIL $case exit=$($p.ExitCode)"
  }
  Add-Content "$out\progress.txt" ("PASS "+$case+" "+(Get-Date).ToString('o'))
 }
 'PASS' | Set-Content "$out\result.txt"
} catch {
 $_.ToString() | Set-Content "$out\result.txt"
 exit 1
}
