# Development-PC test of approute-lib.ps1 (Windows PowerShell 5.1, scratch files only, no registry, no lab):
# router file swap (install, idempotence, rollback, foreign and interrupted states, source admission), policy value
# sets (cpu partial, allowlist, gpu-default, deny_always, readback equality), list parsing, manifest and package
# checks. Prints PASS ... on success, throws otherwise.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$ops=Split-Path -Parent $PSScriptRoot
. "$ops\durable.ps1"
. "$ops\approute-lib.ps1"
# A fresh directory, so that no reading of this run can come from an older one. An empty directory that a
# caller has already made (the quality gate makes one per check) counts as fresh.
if(Test-Path $Out){
 if(@(Get-ChildItem -Force -LiteralPath $Out).Count){throw 'Fresh (empty) directory required'}
}else{
 $null=New-Item -ItemType Directory $Out
}
$cases=0
function Reject([string]$Name,[scriptblock]$Action){$failed=$false;try{& $Action}catch{$failed=$true};if(!$failed){throw "False acceptance: $Name"};$script:cases++}
function Check([string]$Name,[bool]$Ok){if(!$Ok){throw "Check failed: $Name"};$script:cases++}

# ---- router swap
$dir="$Out\swap";$null=New-Item -ItemType Directory $dir
$active="$dir\bc250d3d_router.dll";$source="$dir\pkg-router.dll";$base="$dir\pkg-baseline.dll"
[IO.File]::WriteAllText($active,'baseline');[IO.File]::WriteAllText($base,'baseline');[IO.File]::WriteAllText($source,'candidate')
$B=(Get-FileHash $active).Hash;$C=(Get-FileHash $source).Hash
$x=Install-AppRouter $active $source $B $C 's1'
Check 'install changes the active file' ($x.changed -and (Get-FileHash $active).Hash -eq $C -and (Get-FileHash $x.held).Hash -eq $B)
Check 'install leaves no prepared file' (!(Test-Path "$active.app-route-candidate"))
$x=Install-AppRouter $active $source $B $C 's2'
Check 'install is idempotent' (!$x.changed -and !$x.held)
$x=Restore-AppRouter $active $base $B $C 's3'
Check 'rollback restores the baseline' ($x.changed -and (Get-FileHash $active).Hash -eq $B -and (Get-FileHash $x.held).Hash -eq $C)
$x=Restore-AppRouter $active $base $B $C 's4'
Check 'rollback is idempotent' (!$x.changed)
# Interrupted install: the active file renamed aside, nothing in its place; rollback puts the baseline back.
Move-Item $active "$dir\interrupted.dll"
$x=Restore-AppRouter $active $base $B $C 's5'
Check 'rollback recovers a missing active file' ($x.changed -and !$x.held -and (Get-FileHash $active).Hash -eq $B)
# Interrupted install with a prepared candidate left over: a matching one is replaced, a foreign one refused.
[IO.File]::WriteAllText("$active.app-route-candidate",'candidate')
$x=Install-AppRouter $active $source $B $C 's6'
Check 'install over a matching prepared leftover' ($x.changed -and (Get-FileHash $active).Hash -eq $C)
$null=Restore-AppRouter $active $base $B $C 's7'
[IO.File]::WriteAllText("$active.app-route-candidate",'something else')
Reject 'foreign prepared file' {Install-AppRouter $active $source $B $C 's8'}
Check 'refused install left the baseline' ((Get-FileHash $active).Hash -eq $B)
Remove-Item "$active.app-route-candidate"
[IO.File]::WriteAllText($active,'foreign')
Reject 'install over a foreign active file' {Install-AppRouter $active $source $B $C 's9'}
Reject 'rollback over a foreign active file' {Restore-AppRouter $active $base $B $C 's10'}
Check 'foreign active file preserved' ([IO.File]::ReadAllText($active) -eq 'foreign')
[IO.File]::WriteAllText($active,'baseline');[IO.File]::WriteAllText($source,'bad candidate')
Reject 'package router hash mismatch' {Install-AppRouter $active $source $B $C 's11'}
Check 'source refusal changed nothing' ((Get-FileHash $active).Hash -eq $B -and !(Test-Path "$active.app-route-held-s11"))
[IO.File]::WriteAllText($source,'candidate');[IO.File]::WriteAllText($base,'damaged')
$null=Install-AppRouter $active $source $B $C 's12'
Reject 'rollback from a damaged baseline copy' {Restore-AppRouter $active $base $B $C 's13'}
Check 'damaged baseline copy left the candidate in place' ((Get-FileHash $active).Hash -eq $C)
[IO.File]::WriteAllText($base,'baseline')
$null=Restore-AppRouter $active $base $B $C 's14'
$left=@(Remove-AppRouterLeftovers $active)
Check 'leftovers removed when unmapped' ($left.Count -ge 4 -and !@($left | Where-Object { !$_.removed }).Count -and
  !@(Get-ChildItem $dir | Where-Object { $_.Name -like 'bc250d3d_router.dll.app-route-*' }).Count)

# ---- lists and policy
$l=ConvertTo-AppList 'a.exe, B.exe,,a.EXE ,steamwebhelper.exe'
Check 'list: trimmed, empty dropped, duplicates dropped case-insensitively' (($l -join '|') -ceq 'a.exe|B.exe|steamwebhelper.exe')
Check 'list: empty text is an empty array' ((ConvertTo-AppList '').Count -eq 0)
Reject 'list: path' {ConvertTo-AppList 'C:\x\a.exe'}
Reject 'list: no extension' {ConvertTo-AppList 'notepad'}
Reject 'list: wildcard' {ConvertTo-AppList '*.exe'}
$root='C:\BC250\m14\app-route-001'
$manifest=[pscustomobject]@{gpu=[pscustomobject]@{dir='gpu';shell='amdgpu_wddm_d3d11.dll'};deny_always=@('witcher3.exe','composition-control.exe')}
$d=Get-AppPolicyDesired -Mode cpu -Manifest $manifest -Root $root
Check 'cpu: Mode alone, partial' ($d.partial -and @($d.values.Keys).Count -eq 1 -and $d.values['Mode'].value -ceq 'cpu')
$d=Get-AppPolicyDesired -Mode allowlist -Allow @('d3d11mt.exe') -Deny @('x.exe') -Manifest $manifest -Root $root
Check 'allowlist: full set' (!$d.partial -and (@($d.values.Keys) -join '|') -ceq 'Mode|GpuUmdPath|RouteLogDirectory|Allow|Deny' -and
  $d.values['GpuUmdPath'].value -ceq 'C:\BC250\m14\app-route-001\gpu\amdgpu_wddm_d3d11.dll' -and
  $d.values['RouteLogDirectory'].value -ceq 'C:\BC250\m14\app-route-001\logs' -and
  (@($d.values['Deny'].value) -join '|') -ceq 'x.exe|witcher3.exe|composition-control.exe' -and $d.values['Allow'].kind -ceq 'MultiString')
Reject 'allowlist without names' {Get-AppPolicyDesired -Mode allowlist -Manifest $manifest -Root $root}
$d=Get-AppPolicyDesired -Mode gpu-default -Manifest $manifest -Root $root
Check 'gpu-default: deny_always only, no Allow' (!$d.values.Contains('Allow') -and (@($d.values['Deny'].value) -join '|') -ceq 'witcher3.exe|composition-control.exe')
Reject 'unknown mode' {Get-AppPolicyDesired -Mode GPU -Manifest $manifest -Root $root}
$before=[ordered]@{Mode=@{kind='String';value='allowlist'};Allow=@{kind='MultiString';value=[string[]]@('a.exe')};GpuUmdPath=@{kind='String';value='C:\x.dll'}}
$e=Get-AppPolicyExpected (Get-AppPolicyDesired -Mode cpu -Manifest $manifest -Root $root) $before
Check 'cpu keeps the other values' ((@($e.Keys | Sort-Object) -join '|') -ceq 'Allow|GpuUmdPath|Mode' -and $e['Mode'].value -ceq 'cpu' -and @($e['Allow'].value)[0] -ceq 'a.exe')
$full=Get-AppPolicyDesired -Mode allowlist -Allow @('b.exe') -Manifest $manifest -Root $root
$e=Get-AppPolicyExpected $full $before
Check 'full write drops the old values' (!$e.Contains('Unknown') -and (@($e['Allow'].value) -join '|') -ceq 'b.exe')
$actual=[ordered]@{}; foreach($n in $e.Keys){$actual[$n]=$e[$n]}
Check 'readback equal' (Test-AppPolicyEqual $e $actual)
$actual['Allow']=@{kind='MultiString';value=[string[]]@('B.exe')}
Check 'readback differs by case' (!(Test-AppPolicyEqual $e $actual))
$actual['Allow']=@{kind='String';value='b.exe'}
Check 'readback differs by kind' (!(Test-AppPolicyEqual $e $actual))
$actual.Remove('Allow')
Check 'readback differs by a missing value' (!(Test-AppPolicyEqual $e $actual))
$actual['Allow']=$e['Allow'];$actual['Extra']=@{kind='String';value='x'}
Check 'readback differs by an extra value' (!(Test-AppPolicyEqual $e $actual))

# ---- manifest and package
$pkg="$Out\pkg";$null=New-Item -ItemType Directory "$pkg\gpu"
[IO.File]::WriteAllText("$pkg\gpu\amdgpu_wddm_d3d11.dll",'shell')
$H=(Get-FileHash "$pkg\gpu\amdgpu_wddm_d3d11.dll").Hash
$good=[ordered]@{schema=1;router=[ordered]@{file='bc250d3d_router.dll';sha256=('A'*64);baseline_sha256=('B'*64);baseline_file='baseline\bc250d3d_router.dll';active_path='C:\BC250\m15\gpu-dwm-003\bc250d3d_router.dll'};
 cpu_umd_path='C:\BC250\m15\desktop-umd173-007\bc250d3d.dll';cpu_umd_sha256=('C'*64);
 gpu=[ordered]@{dir='gpu';shell='amdgpu_wddm_d3d11.dll';files=[ordered]@{'amdgpu_wddm_d3d11.dll'=$H}};clients=[ordered]@{'d3d11mt.exe'=('D'*64)};
 deny_always=@('witcher3.exe');policy_key='SOFTWARE\amdgpu-wddm\AppRouter';client_task='Lab-App-Route-Client'}
function Write-Manifest($o){[IO.File]::WriteAllText("$pkg\app-route.json",($o|ConvertTo-Json -Depth 5))}
Write-Manifest $good
$m=Read-AppRouteManifest $pkg
Check 'manifest read' ($m.gpu.shell -ceq 'amdgpu_wddm_d3d11.dll' -and $m.client_task -ceq 'Lab-App-Route-Client')
foreach($bad in @(
  @{name='router equals baseline';edit={param($o)$o.router.baseline_sha256=('A'*64)}},
  @{name='lower-case hash';edit={param($o)$o.router.sha256=('a'*64)}},
  @{name='relative active path';edit={param($o)$o.router.active_path='bc250d3d_router.dll'}},
  @{name='shell not listed';edit={param($o)$o.gpu.shell='other.dll'}},
  @{name='deny_always not an image name';edit={param($o)$o.deny_always=@('C:\x.exe')}},
  @{name='task name trips the gates';edit={param($o)$o.client_task='BC250-App'}},
  @{name='CPU UMD hash malformed';edit={param($o)$o.cpu_umd_sha256='x'}},
  @{name='schema';edit={param($o)$o.schema=2}})){
 $o=(($good|ConvertTo-Json -Depth 5)|ConvertFrom-Json)
 & $bad.edit $o
 Write-Manifest $o
 Reject ('manifest: '+$bad.name) {Read-AppRouteManifest $pkg}
}
Write-Manifest $good
[IO.File]::WriteAllText("$pkg\SHA256SUMS.txt",("{0} *gpu/amdgpu_wddm_d3d11.dll`n{1} *app-route.json`n" -f $H.ToLowerInvariant(),(Get-FileHash "$pkg\app-route.json").Hash.ToLowerInvariant()))
$t=Test-AppRoutePackage $pkg
Check 'package sums match' ($t.files -eq 2 -and !$t.mismatched.Count)
[IO.File]::WriteAllText("$pkg\gpu\amdgpu_wddm_d3d11.dll",'changed')
$t=Test-AppRoutePackage $pkg
Check 'package sums catch a changed file' ((@($t.mismatched) -join '|') -ceq 'gpu/amdgpu_wddm_d3d11.dll')
Remove-Item "$pkg\gpu\amdgpu_wddm_d3d11.dll"
$t=Test-AppRoutePackage $pkg
Check 'package sums catch a missing file' ((@($t.mismatched) -join '|') -ceq 'gpu/amdgpu_wddm_d3d11.dll')
[IO.File]::WriteAllText("$pkg\SHA256SUMS.txt","not a sums line`n")
Reject 'package sums malformed' {Test-AppRoutePackage $pkg}
"PASS approute-lib: $cases cases (router swap, policy value sets and readback, lists, manifest and package checks)"
