param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-stage.ps1"
if(Test-Path $Out){throw 'Use a fresh directory'}
[void](New-Item -ItemType Directory $Out)
$file=Join-Path $Out 'control.txt';$manifest=Join-Path $Out 'stage-manifest.json'
[IO.File]::WriteAllText($file,'original')
@{schema=1;files=@{'control.txt'=(Get-FileHash $file).Hash}}|ConvertTo-Json|Set-Content $manifest
$expected=(Get-FileHash $manifest).Hash
[void](Assert-KmdStage $Out $expected)
function Must-Reject([scriptblock]$Action){$rejected=$false;try{& $Action|Out-Null}catch{$rejected=$true};if(!$rejected){throw 'False stage acceptance'}}
[IO.File]::WriteAllText($file,'changed')
Must-Reject {Assert-KmdStage $Out $expected}
[IO.File]::WriteAllText($manifest,'{}')
Must-Reject {Assert-KmdStage $Out $expected}
foreach($path in @('../outside.txt','/root.txt','C:/foreign.txt')){
 @{schema=1;files=@{$path=('A'*64)}}|ConvertTo-Json|Set-Content $manifest
 $expected=(Get-FileHash $manifest).Hash
 Must-Reject {Assert-KmdStage $Out $expected}
}
@{schema=1;files=@{}}|ConvertTo-Json|Set-Content $manifest
$expected=(Get-FileHash $manifest).Hash
Must-Reject {Assert-KmdStage $Out $expected}
'PASS: exact stage, changed file/manifest, unsafe paths and empty manifest'
