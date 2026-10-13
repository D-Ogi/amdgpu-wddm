$ErrorActionPreference='Stop'
# Load only pure conversion functions from the real lab scripts. Their entry
# points, registry readers, scheduled tasks and GPU commands never execute.
$win=(Resolve-Path "$PSScriptRoot\..").Path
foreach($pair in @(@('d3d12queue\scanout-trial.ps1','Convert-ScanoutState'),@('frameloop\lab\frameloop-task.ps1','Convert-FrameloopState'))){
 $errors=$null;$ast=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $win $pair[0]),[ref]$null,[ref]$errors)
 if($errors.Count){throw $errors[0]}
 $name=$pair[1]
 $function=$ast.Find({param($node)$node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
 if(!$function){throw "Missing pure state reader $name"}
 . ([scriptblock]::Create($function.Extent.Text))
}
$checks=0
function Check($Ok,[string]$Name){$script:checks++;if(!$Ok){throw $Name}}
function Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};Check $failed 'bad state must refuse'}
foreach($source in @('override','factory','code')){
 $json=@{abi=1;error=0;result=@{status='ok';error=0;type=4;value=1;source=$source}}|ConvertTo-Json -Depth 5
 Check ((Convert-ScanoutState $json).source -eq $source) 'effective source retained'
}
foreach($status in @('absent','access','io','schema','incomplete','type')){
 $json=@{abi=1;error=0;result=@{status=$status;error=2;type=4;value=$null}}|ConvertTo-Json -Depth 5
 Reject {Convert-ScanoutState $json}
}
foreach($json in @('{}','null','{bad','{"abi":1,"error":0,"result":{"status":"ok","error":0,"type":4,"value":null}}')){Reject {Convert-ScanoutState $json}}
foreach($value in @('1',$true,2)){
 $json=@{abi=1;error=0;result=@{status='ok';error=0;type=4;value=$value;source='code'}}|ConvertTo-Json -Depth 5
 Reject {Convert-ScanoutState $json}
}
$items=@()
foreach($name in @('CuMode','CuDisableWgp','CuModeConfirmed','CuModeLastApplied')){
 $setting=$name -in @('CuMode','CuDisableWgp')
 $items+=@{path='';name=$name;kind=$(if($setting){'setting'}else{'record'});override=@{status='absent';error=2};effective=@{status='ok';error=0;type=4;value=24;source='factory'};record=@{status='absent';error=2;value=$null}}
}
$snapshot=@{abi=1;error=0;items=$items}
$record=Convert-FrameloopState ($snapshot|ConvertTo-Json -Depth 6)
Check ($record.CuMode -eq 24 -and $record.state.CuMode.source -eq 'factory') 'effective CU state from factory'
Check ($null -eq $record.CuModeConfirmed -and $record.state.CuModeConfirmed.status -eq 'absent') 'absent record remains typed absence'
foreach($status in @('access','io','schema','incomplete','type')){
 $items[0].effective.status=$status
 Reject {Convert-FrameloopState ($snapshot|ConvertTo-Json -Depth 6)}
}
$items[0].effective.status='ok';$items[0].effective.value=$null
Reject {Convert-FrameloopState ($snapshot|ConvertTo-Json -Depth 6)}
Reject {Convert-FrameloopState '{}'}
"Readonly shared state: $checks checks passed"
