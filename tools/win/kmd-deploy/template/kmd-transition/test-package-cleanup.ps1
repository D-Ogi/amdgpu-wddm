$ErrorActionPreference='Stop'
. "$PSScriptRoot\package-cleanup.ps1"
$hash='A'*64
$packages=@(@{name='oem1.inf';sha256=$hash},@{name='oem2.inf';sha256=('B'*64)})
$selected=@(Select-KmdCandidatePackages $packages $hash 'oem2.inf')
if($selected.Count -ne 1 -or $selected[0].name -ne 'oem1.inf'){throw 'Package selection failed'}
if(@(Select-KmdCandidatePackages @() $hash 'oem2.inf').Count){throw 'Empty store mismatch'}
foreach($case in @(@{packages=$packages;active='oem1.inf'},@{packages=@(@{name='../oem1.inf';sha256=$hash});active='oem2.inf'})){
 $rejected=$false;try{Select-KmdCandidatePackages $case.packages $hash $case.active|Out-Null}catch{$rejected=$true}
 if(!$rejected){throw 'Unsafe package selection accepted'}
}
'PASS: exact INF selection, unrelated retention, absent candidate, active/path rejection'
