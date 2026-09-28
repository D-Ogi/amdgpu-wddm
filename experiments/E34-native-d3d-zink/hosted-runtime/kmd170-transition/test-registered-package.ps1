$ErrorActionPreference='Stop'
. "$PSScriptRoot\package-cleanup.ps1"
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
$hash='A'*64
$p=@{name='oem133.inf';sha256=$hash}
if((Select-KmdRegisteredPackage @($p,@{name='oem99.inf';sha256=('B'*64)}) $hash).name -ne 'oem133.inf'){throw 'Wrong package'}
Must-Reject {Select-KmdRegisteredPackage @() $hash}
Must-Reject {Select-KmdRegisteredPackage @($p,$p) $hash}
Must-Reject {Select-KmdRegisteredPackage @(@{name='..\external.inf';sha256=$hash}) $hash}
Must-Reject {Select-KmdRegisteredPackage @($p) 'bad'}
Must-Reject {Select-KmdRegisteredPackage @($p) ('B'*64)}
'PASS: registered identity, missing/duplicate/wrong package and invalid name/hash'
