$ErrorActionPreference='Stop'
. "$PSScriptRoot\legacy-state.ps1"
$checks=0
function Check($Condition,[string]$Name){$script:checks++;if(!$Condition){throw $Name}}
Check ((Get-KmdLegacyPackageSchema "HKR, Parameters, EnableHangBugcheck, 0x00010001, 0`n") -eq 1) 'schema1 sentinel'
foreach($text in @('HKR,Parameters\DefaultsV2,Schema,0x00010001,2','amdgpu-state.bin','AmdgpuStateCatalog')){
 Check ((Get-KmdLegacyPackageSchema $text) -eq 2) 'schema2 classified'
}
$refused=$false;try{Get-KmdLegacyPackageSchema 'malformed'|Out-Null}catch{$refused=$true}
Check $refused 'unknown INF must refuse'
# Exercise production evidence binding with mocked read-only OS/file calls.
$null=Get-Command ConvertTo-Json,Get-FileHash
function Get-Content {
 param([string]$LiteralPath,[switch]$Raw,[string]$ErrorAction)
 if($evidenceCase -eq 'denied'){throw [UnauthorizedAccessException]::new('denied')}
 if($LiteralPath.EndsWith('package-hashes.json')){
  if($evidenceCase -eq 'malformed'){return '{}'}
  return (@{candidate999=@{'bc250kmd.sys'=('A'*64);'bc250kmd.inf'=('B'*64)};rollback998=@{'bc250kmd.sys'=('A'*64);'bc250kmd.inf'=('B'*64)}}|ConvertTo-Json)
 }
 if($evidenceCase -eq 'schema2'){return 'HKR,Parameters\DefaultsV2,Schema,0x00010001,2'}
 return 'HKR, Parameters, EnableHangBugcheck, 0x00010001, 0'
}
function Get-FileHash {
 param([string]$LiteralPath,[string]$Algorithm,[string]$ErrorAction)
 $hash=if($LiteralPath.EndsWith('.inf')){'B'*64}else{'A'*64}
 if($evidenceCase -eq 'pin-mismatch' -or ($evidenceCase -eq 'unknown-image' -and $LiteralPath.EndsWith('active.sys'))){$hash='C'*64}
 return [pscustomobject]@{Hash=$hash}
}
function Get-ItemProperty {
 param([string]$LiteralPath,[string]$Name,[string]$ErrorAction)
 if($LiteralPath -ne 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -or $Name -ne 'ImagePath'){throw 'Unexpected registry read'}
 return [pscustomobject]@{ImagePath='P:\fixture\active.sys'}
}
foreach($evidenceCase in @('legacy','schema2','denied','malformed','unknown-image','pin-mismatch')){
 $mutated=$false;$problem=''
 try{Assert-KmdLegacyState 'P:\fixture';$mutated=$true}catch{$problem=$_.Exception.Message}
 Check ($mutated -eq ($evidenceCase -eq 'legacy')) "$evidenceCase package/image evidence before mutation: $problem"
}
Remove-Item Function:Get-Content,Function:Get-FileHash,Function:Get-ItemProperty
# Execute the actual gate before a mutation witness. No registry, SCM or GPU.
foreach($case in @('legacy','schema2','denied','malformed','empty')){
 function Get-KmdLegacyStateEvidence {
  param([string]$Directory)
  switch($case){
   legacy {return @(1,1)}
   schema2 {return @(1,2)}
   denied {throw [UnauthorizedAccessException]::new('denied')}
   malformed {return @('1',1)}
   empty {return @()}
  }
 }
 $mutated=$false;$refused=$false
 try{Assert-KmdLegacyState 'unused';$mutated=$true}catch{$refused=$_.Exception.Message -like 'Legacy transition refused:*'}
 Check ($mutated -eq ($case -eq 'legacy')) "$case must stop before mutation"
 Check ($refused -eq ($case -ne 'legacy')) "$case refusal is explicit"
}
# Every independently callable transition entry performs admission first.
foreach($name in @('preflight','postflight','phase','detector-phase','dispatch','launch','watch','worker','setup-log-phase')){
 $ast=[System.Management.Automation.Language.Parser]::ParseFile("$PSScriptRoot\$name.ps1",[ref]$null,[ref]$null)
 $statements=@($ast.EndBlock.Statements)
 Check ($statements[1].Extent.Text -eq '. "$PSScriptRoot\legacy-state.ps1"') "$name loads shared guard before work"
 Check ($statements[2].Extent.Text -match '^Assert-KmdLegacyState( |$)') "$name guards before work"
}
"PASS: legacy state admission, $checks checks"
