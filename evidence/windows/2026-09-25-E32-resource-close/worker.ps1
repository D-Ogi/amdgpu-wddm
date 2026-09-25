param([string]$Out,[int]$DurationSeconds=86400,[int]$MaxCycles=0)
$ErrorActionPreference='Stop'
$env:VK_DRIVER_FILES='C:\BC250\m10\wsi-final\radeon_icd.json'
$env:VK_ICD_FILENAMES=$env:VK_DRIVER_FILES
$env:VK_LOADER_DEBUG='driver'
$env:BC250_TRACE_SUBMITS='0'
$env:MESA_VK_WSI_DEBUG=''
$env:PATH='C:\BC250\m8;'+$env:PATH
Add-Type 'using System; using System.Runtime.InteropServices; public class M11Power { [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags); }'
[M11Power]::SetThreadExecutionState(2147483651) | Out-Null
$watch=[Diagnostics.Stopwatch]::StartNew()
$child=$null
function SaveJson($name,$obj){
 $path=Join-Path $Out $name
 [IO.File]::WriteAllText("$path.tmp",($obj | ConvertTo-Json -Depth 8 -Compress),[Text.UTF8Encoding]::new($false))
 if([IO.File]::Exists($path)){[IO.File]::Replace("$path.tmp",$path,[NullString]::Value)}else{[IO.File]::Move("$path.tmp",$path)}
}
function RunNative($name,$exe,$arguments,$folder) {
 if(Test-Path "$Out\stop.txt"){throw 'Monitor stop'}
 if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
 $script:child=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory ([IO.Path]::GetDirectoryName($exe)) -WindowStyle Normal -PassThru -RedirectStandardOutput "$folder\$name.out" -RedirectStandardError "$folder\$name.err"
 $keep=$script:child.Handle
 SaveJson 'active.json' @{pid=$script:child.Id;exe=$exe;phase=$name;cycle=$cycle;utc=[DateTime]::UtcNow.ToString('o')}
 if(-not $script:child.WaitForExit(120000)){Stop-Process -Id $script:child.Id -Force;throw "Timeout $name"}
 $code=$script:child.ExitCode
 $script:child=$null
 if($code -ne 0){throw "Native failure $name exit=$code"}
 $err=[IO.File]::ReadAllText("$folder\$name.err")
 if($err -notmatch 'C:\\BC250\\m10\\wsi-final\\(?:\.\\)?vulkan_radeon.dll'){throw "ICD witness missing $name"}
 return [IO.File]::ReadAllText("$folder\$name.out")
}
try {
 SaveJson 'worker-start.json' @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;duration=$DurationSeconds;max_cycles=$MaxCycles}
 $reference=Get-Content C:\BC250\m11\compute-reference.json -Raw | ConvertFrom-Json
 $cycle=0
 do {
  $cycle++
  $folder=Join-Path $Out ('cycle-{0:D6}' -f $cycle)
  New-Item -ItemType Directory $folder | Out-Null
  $compute=RunNative 'compute' 'C:\BC250\m8\vkcompute.exe' @('C:\BC250\m8\spv','--runs','3') $folder
  if($compute -notmatch '8 test\(s\) run, 0 mismatch\(es\)'){throw 'Compute suite count/content failed'}
  foreach($item in $reference.PSObject.Properties) {
   $pattern='(?m)^'+[regex]::Escape($item.Name)+'\s+n=\d+\s+hash=0x'+$item.Value+'\s+cpu_hash=0x'+$item.Value+'\s+match=yes'
   if($compute -notmatch $pattern){throw "Linux hash mismatch $($item.Name)"}
  }
  foreach($model in @('stories15M','tinyllama')) {
   if($model -eq 'stories15M'){$file='stories15M-q4_0.gguf';$prompt='"Once upon a time"';$tokens=96}
   else {$file='tinyllama-1.1b-chat-v1.0.Q4_0.gguf';$prompt='"The capital of France is"';$tokens=64}
   $actual=RunNative $model 'C:\BC250\m9\llama\llama-completion.exe' @('-m',"C:\BC250\m9\models\$file",'-p',$prompt,'-n',"$tokens",'--temp','0','--seed','1','-ngl','99','-no-cnv','-t','6','--verbose') $folder
   $expected=[IO.File]::ReadAllText("C:\BC250\m11\$model-ngl99.out")
   if($actual.Replace([string][char]13,'') -cne $expected.Replace([string][char]13,'')){throw "Model content mismatch $model"}
   $err=[IO.File]::ReadAllText("$folder\$model.err")
   $m=[regex]::Match($err,'offloaded (\d+)/(\d+) layers to GPU')
   if(-not $m.Success -or $m.Groups[1].Value -ne $m.Groups[2].Value){throw "Incomplete GPU offload $model"}
  }
  $null=RunNative 'cube' 'C:\BC250\m10\wsi-final\vkcube.exe' @('--c','600','--width','640','--height','480','--suppress_popups') $folder
  $hashes=@{}
  foreach($name in @('compute','stories15M','tinyllama')){$hashes[$name]=(Get-FileHash "$folder\$name.out").Hash}
  SaveJson 'active.json' @{pid=0;phase='checkpoint';cycle=$cycle;utc=[DateTime]::UtcNow.ToString('o')}
  [IO.File]::WriteAllText("$Out\checkpoint.request",[string]$cycle)
  $gate=[Diagnostics.Stopwatch]::StartNew()
  do {
   if(Test-Path "$Out\stop.txt"){throw 'Monitor stop at checkpoint'}
   if($gate.Elapsed.TotalSeconds -gt 45){throw 'Monitor checkpoint timeout'}
   Start-Sleep -Milliseconds 200
  } while(-not(Test-Path "$Out\checkpoint.ack") -or [IO.File]::ReadAllText("$Out\checkpoint.ack").Trim() -ne [string]$cycle)
  $record=@{cycle=$cycle;utc=[DateTime]::UtcNow.ToString('o');elapsed_seconds=$watch.Elapsed.TotalSeconds;stdout_file_sha256=$hashes;compute_reference_hashes_checked=$reference;result='PASS'}
  $record | ConvertTo-Json -Compress | Add-Content "$Out\cycles.jsonl" -Encoding UTF8
  SaveJson 'progress.json' $record
 } while($watch.Elapsed.TotalSeconds -lt $DurationSeconds -and ($MaxCycles -eq 0 -or $cycle -lt $MaxCycles))
 SaveJson 'worker-result.json' @{status='PASS';cycles=$cycle;elapsed_seconds=$watch.Elapsed.TotalSeconds;utc=[DateTime]::UtcNow.ToString('o')}
} catch {
 SaveJson 'worker-result.json' @{status='FAIL';message=$_.ToString();cycle=$cycle;elapsed_seconds=$watch.Elapsed.TotalSeconds;utc=[DateTime]::UtcNow.ToString('o')}
 exit 1
} finally {
 if($child -and -not $child.HasExited){Stop-Process -Id $child.Id -Force}
 [M11Power]::SetThreadExecutionState(2147483648) | Out-Null
}
