param([Parameter(Mandatory=$true)][string]$Out,[ValidateSet('Positive','Stuck')][string]$Mode)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$stage=Join-Path $Out $Mode.ToLowerInvariant()
if(Test-Path "$stage\started.json"){throw 'This stage already ran; never retry a stuck dispatch'}
New-Item -ItemType Directory -Force $stage | Out-Null
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$exe='C:\BC250\m8\vkcompute.exe'
if((Get-FileHash $exe).Hash -ne '70F4F3FCCF2CC2D9668F9953021C5D4C1F50EEEABAB7D13901F97810C8FFAD00'){throw 'Harness identity changed'}
$env:VK_DRIVER_FILES='C:\BC250\m10\wsi-final\radeon_icd.json'
$env:VK_ICD_FILENAMES=$env:VK_DRIVER_FILES
$env:VK_LOADER_DEBUG='driver'
$env:BC250_TRACE_SUBMITS='1'
$env:PATH='C:\BC250\m8;'+$env:PATH
if((Get-FileHash C:\BC250\m10\wsi-final\vulkan_radeon.dll).Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){throw 'ICD changed'}
if($Mode -eq 'Stuck'){
 $positive=Get-Content "$Out\positive\result.json" -Raw | ConvertFrom-Json
 if($positive.status -ne 'PASS'){throw 'Positive fill control required'}
 if((Get-FileHash "$Out\shader\fill.spv").Hash -ne '81D2FD3253A6FFA39B304D165477166E697F08542EEEB12D17CEB5B87EABDCCE'){throw 'Stuck shader changed'}
 $shaders="$Out\shader"
} else {
 $shaders='C:\BC250\m8\spv'
}
function Save($name,$value){
 [IO.File]::WriteAllText("$stage\$name.json",($value | ConvertTo-Json -Depth 5 -Compress),[Text.UTF8Encoding]::new($false))
}
Save 'started' @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;mode=$Mode;shader_sha256=(Get-FileHash "$shaders\fill.spv").Hash}
# Exactly one selected test, one submission run. No application-level retry.
$child=Start-Process -FilePath $exe -ArgumentList @($shaders,'--only','fill_g1','--runs','1') -WorkingDirectory C:\BC250\m8 -WindowStyle Hidden -PassThru -RedirectStandardOutput "$stage\native.out" -RedirectStandardError "$stage\native.err"
$keep=$child.Handle
Save 'child' @{pid=$child.Id;exe=$exe;utc=[DateTime]::UtcNow.ToString('o')}
if(-not $child.WaitForExit(30000)){
 Save 'result' @{status='PROCESS_DEADLINE';utc=[DateTime]::UtcNow.ToString('o');note='No GPU recovery claim'}
 # This may itself be blocked by the GPU. The host observer has its own deadline.
 Stop-Process -Id $child.Id -Force
 exit 2
}
$code=$child.ExitCode
$stdout=[IO.File]::ReadAllText("$stage\native.out")
$stderr=[IO.File]::ReadAllText("$stage\native.err")
$witness=$stderr -match 'C:\\BC250\\m10\\wsi-final\\(?:\.\\)?vulkan_radeon.dll'
if($Mode -eq 'Positive'){
 $ok=$code -eq 0 -and $witness -and
  $stdout -match 'fill_g1\s+n=\d+\s+hash=0x7018cdd513a22325\s+cpu_hash=0x7018cdd513a22325\s+match=yes' -and
  $stdout -match '1 test\(s\) run, 0 mismatch\(es\)'
 $status=if($ok){'PASS'}else{'FAIL'}
} else {
 $status=if($code -eq 0){'UNEXPECTED_COMPLETION'}else{'OBSERVED_NATIVE_EXIT'}
}
Save 'result' @{status=$status;native_exit=$code;icd_witness=$witness;utc=[DateTime]::UtcNow.ToString('o');note='Process exit alone does not prove GPU recovery'}
if($Mode -eq 'Positive' -and -not $ok){exit 1}
