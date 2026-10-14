# Explicit bridge owned by the new engine. A legacy engine never runs its install
# phase after restoration: it would rewrite the state which this bridge preserves.
function Get-LegacyRollbackTarget {
    param([string]$PackageRoot,[string]$FirmwareDir)
    $integrity=Test-PackageManifest -PackageRoot $PackageRoot
    if(-not $integrity.ok){throw "rollback package integrity: $($integrity.detail)"}
    $compat=Test-CompatibilityRecord -PackageRoot $PackageRoot -FirmwareDir $FirmwareDir -CheckedManifest $integrity.manifest
    if(-not $compat.ok){throw "rollback package contract: $($compat.detail)"}
    $inf=Join-Path $PackageRoot 'payload\kmd\bc250kmd.inf'
    if($compat.record.no_live_rebind.method -ne 'di-install-as-set' -or
       [IO.File]::ReadAllText($inf) -match '(?im)^\s*HKR\s*,\s*,\s*AmdgpuStateCatalog\s*,') {
        throw 'legacy rollback requires a schema 1 package with the recorded INSTALL_AS_SET method'
    }
    # The converter is for this catalog's pre-migration KMD ABI. A different
    # legacy reader needs a separately reviewed conversion profile.
    if([string]$integrity.manifest.kmd_abi -ine '0x000700D8') {throw 'unsupported legacy rollback KMD ABI'}
    [pscustomobject]@{manifest=$integrity.manifest;product=(Get-ProductPackageQuad $integrity.manifest.version);
        inf_identity=(Get-InfDriverIdentity ([IO.File]::ReadAllText($inf)));
        inf_sha256=(Get-Sha256 $inf);sys_sha256=(Get-Sha256 (Join-Path $PackageRoot 'payload\kmd\bc250kmd.sys'));
        manifest_sha256=(Get-Sha256 (Join-Path $PackageRoot 'manifest.json'))}
}
function Invoke-LegacyRollbackWorker {
    param([string]$Worker,[string]$Operation,$Record,[scriptblock]$Runner)
    if($Operation -notin @('capture','restore','enable','check','remove')){throw 'invalid rollback operation'}
    if($Record.transaction -cnotmatch '\A[0-9a-f]{32}\z' -or $Record.transaction -eq ('0'*32) -or
       $Record.device -notmatch '\APCI\\[A-Za-z0-9_&\\-]+\z'){throw 'invalid rollback transaction identity'}
    [void](ConvertTo-DriverQuad $Record.product)
    foreach($hash in @($Record.inf_sha256,$Record.sys_sha256,$Record.manifest_sha256)) {
        if($hash -notmatch '\A[0-9a-fA-F]{64}\z'){throw 'invalid rollback package identity'}
    }
    if(-not $Runner){$Runner={param($exe,$arguments) Invoke-Native $exe $arguments}}
    $response=& $Runner $Worker @("rollback-$Operation",$Record.transaction,$Record.product,$Record.device,
        $Record.inf_sha256.ToLowerInvariant(),$Record.sys_sha256.ToLowerInvariant(),$Record.manifest_sha256.ToLowerInvariant())
    if($null -eq $response -or $response.code -ne 0){throw "rollback $Operation failed: $($response.text)"}
    $reply=$response.text | ConvertFrom-Json -ErrorAction Stop
    if($reply.abi -ne 1 -or $null -eq $reply.error -or $reply.error -ne 0){throw "rollback $Operation lacks durable acknowledgment"}
}
function Remove-LegacyRollbackDriverSets {
    param($State,[string]$Worker,[scriptblock]$Inventory,[scriptblock]$Hash,[scriptblock]$Runner)
    if(-not $Inventory){$Inventory={Get-OurDriverPackageList}}
    if(-not $Hash){$Hash={param($name) Get-Sha256 (Join-Path $env:windir "INF\$name")}}
    $r=$State.legacy_rollback
    if(-not $r -or $r.phase -ne 'captured'){throw 'driver-set removal requires a final protected rollback capture'}
    Invoke-LegacyRollbackWorker -Worker $Worker -Operation check -Record $r -Runner $Runner
    # Inventory never authorizes removal. The worker removes only the formerly
    # bound INF whose name and hash it captured itself. Competing ranks refuse.
    foreach($p in @(& $Inventory)) {
        if($p.published -notmatch '\Aoem[0-9]+\.inf\z'){throw 'untrusted rollback driver-store inventory'}
        if($p.published -ieq $r.source_inf -or (& $Hash $p.published) -ieq $r.inf_sha256){continue}
        $date=ConvertTo-DriverDate ([string]$p.date);$quad=ConvertTo-DriverQuad ([string]$p.version)
        $targetDate=ConvertTo-DriverDate ([string]$r.target_date);$targetQuad=ConvertTo-DriverQuad ([string]$r.target_quad)
        if($date -gt $targetDate -or ($date -eq $targetDate -and $quad -ge $targetQuad)) {
            throw "rollback blocked by competing package $($p.published); remove that explicitly identified package separately before resuming"
        }
    }
    Invoke-LegacyRollbackWorker -Worker $Worker -Operation remove -Record $r -Runner $Runner
    if(@(& $Inventory | Where-Object {$_.published -ieq $r.source_inf}).Count) {
        throw 'the captured source INF still awaits removal; keep the device disabled, restart, and resume this rollback'
    }
    Invoke-LegacyRollbackWorker -Worker $Worker -Operation check -Record $r -Runner $Runner
}
function Start-LegacyRollback {
    param($State,$Target,[string]$Device,[string]$Worker,[scriptblock]$Save,[scriptblock]$Runner,[string]$SourceInf)
    if($State.driver_state -and $State.driver_state.phase -ne 'acknowledged'){throw 'finish the pending state migration before rollback'}
    $r=$State.legacy_rollback
    if(-not $r){
        $r=[pscustomobject]@{schema=1;transaction=[guid]::NewGuid().ToString('N');phase='capture';device=$Device;
            product=$Target.product;inf_sha256=$Target.inf_sha256;sys_sha256=$Target.sys_sha256;manifest_sha256=$Target.manifest_sha256;
            source_inf=$SourceInf;target_date=$Target.inf_identity.date;target_quad=$Target.inf_identity.quad}
        if($SourceInf -notmatch '\Aoem[0-9]+\.inf\z'){throw 'rollback cannot identify the currently bound source INF'}
        Set-StateValue $State 'legacy_rollback' $r; & $Save $State
    }
    foreach($key in 'product','inf_sha256','sys_sha256','manifest_sha256') {
        if($r.$key -ine $Target.$key){throw 'resume the exact rollback package recorded before capture'}
    }
    if($r.schema -ne 1 -or $r.device -cne $Device -or $r.phase -notin @('capture','captured','restored','enabled')){throw 'invalid rollback receipt'}
    # The native receipt is authoritative even if state.json claims this passed.
    Invoke-LegacyRollbackWorker -Worker $Worker -Operation capture -Record $r -Runner $Runner
    if($r.phase -eq 'capture'){$r.phase='captured'; & $Save $State}
    return $r
}
function Complete-LegacyRollback {
    param($State,[string]$Worker,[ValidateSet('restore','enable')][string]$Operation,[scriptblock]$Save,[scriptblock]$Runner)
    $r=$State.legacy_rollback
    if(-not $r -or $r.phase -notin @('captured','restored','enabled')){throw 'rollback has no final capture'}
    if($Operation -eq 'enable' -and $r.phase -notin @('restored','enabled')){throw 'restore legacy state before enabling the device'}
    Invoke-LegacyRollbackWorker -Worker $Worker -Operation $Operation -Record $r -Runner $Runner
    $r.phase=if($Operation -eq 'restore'){'restored'}else{'enabled'}; & $Save $State
}
