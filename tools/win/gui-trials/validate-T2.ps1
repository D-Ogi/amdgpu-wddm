# Local-only static/pure validation. Never dot-sources or runs a trial entry point.
#
#   pwsh -NoProfile -File tools\win\gui-trials\validate-T2.ps1
#
# -TempDirectory holds the in-memory evidence helpers' scratch files; it stays outside this repository
# (BC250_TEST_OUT, as the quality gate sets it, otherwise the system temporary directory).
param(
    [string]$Directory=$PSScriptRoot,
    [string]$TempDirectory=''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2.0
if (-not $TempDirectory) {
    $base=if ($env:BC250_TEST_OUT) { $env:BC250_TEST_OUT } else { [IO.Path]::GetTempPath() }
    $TempDirectory=Join-Path $base 'gui-trial-validate'
}
$env:TEMP=$TempDirectory;$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
Add-Type -AssemblyName System.Numerics
$script:Checks=0
function Assert-Check([bool]$Condition,[string]$Name) {
    if(-not $Condition){throw ('FAIL: '+$Name)}
    $script:Checks++
}
function Assert-Throws([scriptblock]$Action,[string]$Name) {
    $threw=$false;try{& $Action | Out-Null}catch{$threw=$true}
    Assert-Check $threw $Name
}
$names=@('L1.ps1','L3.ps1','L4.ps1','L5.ps1','L-CU.ps1','L-OFF.ps1','gui-trial-common.ps1','gui-trial-firewall.ps1')
$script:Asts=@{}
foreach($name in $names) {
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $Directory $name),[ref]$tokens,[ref]$errors)
    Assert-Check (@($errors).Count -eq 0) ('parse '+$name)
    $script:Asts[$name]=$ast
}
function Import-PureFunction([string]$Name,[string]$File) {
    $node=$script:Asts[$File].Find({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst] -and $a.Name -ceq $Name},$true)
    if(-not $node){throw ('Missing function '+$Name)}
    # Explicit whitelist below only; no entry point, firewall mutator, network or process helper imported.
    $parameters=@($node.Parameters|ForEach-Object {$_.Extent.Text}) -join ','
    Set-Item -LiteralPath ('Function:script:'+$Name) -Value ([scriptblock]::Create(('param('+$parameters+')'+[char]10+$node.Body.Extent.Text.Substring(1,$node.Body.Extent.Text.Length-2))))
}
foreach($n in @('Get-GuiField','Get-GuiStages','Test-GuiMutating','ConvertTo-GuiArgument','ConvertFrom-GuiTemperatureText','Assert-GuiMutationClosure')){
    Import-PureFunction $n 'gui-trial-common.ps1'
}
foreach($n in @('ConvertTo-GuiIpNumber','ConvertFrom-GuiIpNumber','Get-GuiCidr','Get-GuiOfflineRanges','Get-GuiFirewallNames')){
    Import-PureFunction $n 'gui-trial-firewall.ps1'
}
function Get-NetIPAddress {param($AddressState,$ErrorAction)
    @([pscustomobject]@{IPAddress='10.23.45.10';PrefixLength=24},[pscustomobject]@{IPAddress='fd12:3456:789a:1::10';PrefixLength=64})
}
$c=[pscustomobject]@{trial_id='fixture-001';offline=[pscustomobject]@{local_subnets=@('10.23.45.0/24','fd12:3456:789a:1::/64');management_address='10.23.45.20';external_probe_address='1.1.1.1';external_probe_port=443}}
$rules=@(Get-GuiFirewallNames $c)
Assert-Check ($rules.Count -eq 2) 'two exact firewall rule names'
Assert-Check ($rules[0] -ceq 'BC250-GUI-fixture-001-offline-block') 'block exact name'
Assert-Check ($rules[1] -ceq 'BC250-GUI-fixture-001-offline-local') 'allow exact name'
$ranges=Get-GuiOfflineRanges $c
function Test-Blocked([string]$Ip) {
    $n=ConvertTo-GuiIpNumber $Ip;$bits=([Net.IPAddress]::Parse($Ip)).GetAddressBytes().Length*8
    foreach($range in $ranges.blocked) {
        $parts=$range.Split('-')
        if(([Net.IPAddress]::Parse($parts[0])).GetAddressBytes().Length*8 -eq $bits -and
            $n -ge (ConvertTo-GuiIpNumber $parts[0]) -and $n -le (ConvertTo-GuiIpNumber $parts[1])){return $true}
    }
    return $false
}
foreach($ip in @('0.0.0.0','1.1.1.1','10.23.44.255','10.23.46.0','255.255.255.255','::','::2','2001:4860:4860::8888','ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff')){
    Assert-Check (Test-Blocked $ip) ('nonlocal blocked '+$ip)
}
foreach($ip in @('10.23.45.0','10.23.45.20','10.23.45.255','127.0.0.1','127.255.255.255','::1','fd12:3456:789a:1::','fd12:3456:789a:1:ffff:ffff:ffff:ffff')){
    Assert-Check (-not (Test-Blocked $ip)) ('local preserved '+$ip)
}
$c.offline.local_subnets=@('10.0.0.0/8')
Assert-Throws {Get-GuiOfflineRanges $c} 'reject subnet wider than interface prefix'
$sample='dpm version=0x000700C6 flags=0x3 temperature_c=86.9 gfx_mhz=1500 busy_pct=1.0 avg_pct=1.0 src=grbm submit_pct=0.0'
Assert-Check ((ConvertFrom-GuiTemperatureText $sample) -eq 86.9) 'real telemetry format'
Assert-Check ((ConvertFrom-GuiTemperatureText ($sample.Replace('86.9','87.0'))) -ge 87) '87C stops inclusively'
Assert-Throws {ConvertFrom-GuiTemperatureText ($sample.Replace('86.9','86.9(stale)'))} 'stale temperature refused'
Assert-Throws {ConvertFrom-GuiTemperatureText 'vram segments=1'} 'missing temperature refused'
Assert-Throws {ConvertFrom-GuiTemperatureText ($sample+[char]10+$sample)} 'ambiguous multiple samples refused'
Assert-Throws {ConvertFrom-GuiTemperatureText ($sample.Replace('86.9','999.0'))} 'implausible temperature refused'
Assert-Check ((Get-GuiStages 'L-CU').Count -eq 6) 'six bounded CU stages'
Assert-Check (Test-GuiMutating ([pscustomobject]@{trial='L5';stage='resume'})) 'resume requires mutation admission'
Assert-Check (-not (Test-GuiMutating ([pscustomobject]@{trial='L-CU';stage='observe-40'}))) 'observation is read-only'
Assert-Check ((ConvertTo-GuiArgument 'C:\a b\') -ceq '"C:\a b\\"') 'native trailing slash quoting'
# Real closure validator, only its file reads/hash checks replaced with in-memory evidence.
function Assert-GuiArtifact($Artifact) {
    if($null -eq $Artifact -or -not $script:Evidence.ContainsKey($Artifact.path)){throw 'Missing evidence'}
}
function Read-GuiJson([string]$Path) {
    if(-not $script:Evidence.ContainsKey($Path)){throw 'Missing evidence'}
    return $script:Evidence[$Path]
}
$mc=[pscustomobject]@{trial_id='mut-1';stage='repair'}
$script:Evidence=@{
    'closed'=[pscustomobject]@{schema=1;trial_id='mut-1';stage='repair';boot_id='boot-a';elevated_children_closed=$true}
    'durable'=[pscustomobject]@{schema=1;trial_id='mut-1';stage='repair';boot_id='boot-a';safe_boundary=$true}
}
$mr=[pscustomobject]@{schema=1;trial_id='mut-1';stage='repair';boot_id='boot-a';outcome='cancelled';tree_closed=$true;
    elevated_children_closed=$true;closure_evidence=[pscustomobject]@{path='closed'};durable_boundary_evidence=[pscustomobject]@{path='durable'}}
Assert-GuiMutationClosure $mc $mr 'boot-a';Assert-Check $true 'cancelled mutation with current closure accepted'
$mr.elevated_children_closed=$false
Assert-Throws {Assert-GuiMutationClosure $mc $mr 'boot-a'} 'cancelled worker job alone never closes engine'
$mr.elevated_children_closed=$true;$mr.boot_id='old'
Assert-Throws {Assert-GuiMutationClosure $mc $mr 'boot-a'} 'old boot terminal refused'
$mr.boot_id='boot-a';$script:Evidence['durable'].safe_boundary=$false
Assert-Throws {Assert-GuiMutationClosure $mc $mr 'boot-a'} 'unsafe cancel boundary refused'
$script:Evidence['durable'].safe_boundary=$true;$script:Evidence.Remove('closed')
Assert-Throws {Assert-GuiMutationClosure $mc $mr 'boot-a'} 'missing closure file refused'
# Static ordering checks supplement pure checks without invoking orchestration or firewall cmdlets.
$common=[IO.File]::ReadAllText((Join-Path $Directory 'gui-trial-common.ps1')).Replace([string][char]13,'')
$firewall=[IO.File]::ReadAllText((Join-Path $Directory 'gui-trial-firewall.ps1'))
Assert-Check ($common.IndexOf('Assert-GuiMutationClosure $C $mutationResult $boot') -lt $common.IndexOf("if("+ '$stopReason'+")"+'{$status='+"'stopped'")) 'mutation closure before stopped branch'
Assert-Check ($common.Contains('$closure=$true; $offlineClean=$true')) 'no-child initial closure'
Assert-Check ($common.Contains('$closure=$false'+[char]10+"        "+'$child=Start-GuiBounded')) 'closure becomes unknown before launch'
Assert-Check ($common.Contains('$e.can_cleanup=')) 'host cleanup readiness exposed'
Assert-Check ($common.IndexOf("Write-GuiJson ("+'$C.directory'+"+'\prepared.json')") -lt $common.IndexOf('            Register-GuiTask $C $Script $ConfigPath')) 'preparation identity before task mutation'
Assert-Check ($firewall.Contains('function Enter-GuiFirewallLock') -and $firewall.Contains('offline.closed')) 'serialized closed lease'
Assert-Check ($firewall.Contains('$r.Group -cne $owner.group -or $r.Description -cne $owner.description')) 'exact ownership required for removal'
Assert-Check ($firewall.Contains("if(-not (Test-Path -LiteralPath "+'$ownedPath'+")){return "+'$true'+"}")) 'no owned journal preserves collisions'
Assert-Check (-not $firewall.Contains('Set-NetFirewallProfile')) 'existing default policy unchanged'
[pscustomobject]@{schema=1;status='passed';checks=$script:Checks;powershell=$PSVersionTable.PSVersion.ToString();scope='AST plus pure helpers and in-memory evidence; no trial entry point, task, firewall, network, process launch, GUI or lab invoked'}|ConvertTo-Json -Compress
