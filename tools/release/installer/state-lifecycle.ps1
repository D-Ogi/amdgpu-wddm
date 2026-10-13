# State schema 2 installer transaction. PowerShell owns orchestration only; the
# bounded SYSTEM worker owns capture, freeze, protected files and acknowledgment.
function Invoke-DriverStateWorker {
    param([string]$Worker,[ValidateSet('prepare','ack','archive')][string]$Operation,
        [string]$Transaction,[string]$Product,[string]$Device,[scriptblock]$Runner)
    if($Transaction -cnotmatch '\A[0-9a-f]{32}\z' -or $Transaction -eq ('0'*32)) { throw 'invalid state transaction identity' }
    [void](ConvertTo-DriverQuad $Product)
    if($Device -notmatch '\APCI\\[A-Za-z0-9_&\\-]+\z') { throw 'invalid state device identity' }
    if(-not $Runner) { $Runner={param($exe,$arguments) Invoke-Native $exe $arguments} }
    $reply=& $Runner $Worker @($Operation,$Transaction,$Product,$Device)
    if($null -eq $reply -or $reply.code -ne 0) { throw "state $Operation failed (exit $($reply.code)): $($reply.text)" }
    try { $json=$reply.text | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "invalid state worker response for $Operation" }
    if($json.abi -ne 1 -or $null -eq $json.error -or $json.error -ne 0) { throw "state $Operation did not confirm durable success" }
}

function Start-DriverStateTransaction {
    param($State,[string]$Worker,[string]$Product,[string]$Device,[string]$InfSha256,
        [scriptblock]$Save,[scriptblock]$Runner)
    $record=$State.driver_state
    if($record -and $record.phase -eq 'prepared') {
        if($record.product -cne $Product -or $record.device -cne $Device -or $record.inf_sha256 -ine $InfSha256) {
            throw 'another driver state transaction is pending; resume its exact package first'
        }
    } else {
        if($record -and $record.phase -ne 'acknowledged') { throw 'invalid driver state transaction phase' }
        $record=[pscustomobject]@{schema=2;transaction=[guid]::NewGuid().ToString('N');phase='prepared';
            product=$Product;device=$Device;inf_sha256=$InfSha256}
        Set-StateValue $State 'driver_state' $record
        # Identity reaches durable installer state before the worker can freeze.
        & $Save $State
    }
    Invoke-DriverStateWorker -Worker $Worker -Operation prepare -Transaction $record.transaction -Product $Product -Device $Device -Runner $Runner
    return $record
}

function Complete-DriverStateTransaction {
    param($State,[string]$Worker,[string]$Product,[string]$Device,[string]$InfSha256,
        [scriptblock]$Save,[scriptblock]$Runner)
    $record=$State.driver_state
    if(-not $record -or $record.schema -ne 2 -or $record.phase -notin @('prepared','acknowledged') -or
       $record.product -cne $Product -or $record.device -cne $Device -or $record.inf_sha256 -ine $InfSha256) {
        throw 'driver binding has no matching prepared state transaction'
    }
    # The native operation checks Active/Ready/Catalog with the common adapter,
    # archives first, and removes only the matching pending file by its handle.
    Invoke-DriverStateWorker -Worker $Worker -Operation ack -Transaction $record.transaction -Product $Product -Device $Device -Runner $Runner
    $record.phase='acknowledged'; & $Save $State
}

function Save-DriverStateBeforeRemoval {
    param($State,[string]$Worker,[string]$Product,[string]$Device,[scriptblock]$Save,[scriptblock]$Runner)
    if($State.driver_state -and $State.driver_state.phase -ne 'acknowledged') {
        throw 'uninstall requires the pending install transaction to finish first; no service data was removed'
    }
    $record=$State.driver_state_removal
    if(-not $record) {
        $record=[pscustomobject]@{schema=2;transaction=[guid]::NewGuid().ToString('N');phase='capture';product=$Product;device=$Device}
        Set-StateValue $State 'driver_state_removal' $record; & $Save $State
    }
    if($record.schema -ne 2 -or $record.product -cne $Product -or $record.device -cne $Device -or
       $record.phase -notin @('capture','archived')) { throw 'removal receipt does not match this device and package' }
    # Recheck even an archived receipt. The helper checks stopped state and exact
    # snapshot bytes, so editing state.json cannot authorize service deletion.
    Invoke-DriverStateWorker -Worker $Worker -Operation archive -Transaction $record.transaction -Product $Product -Device $Device -Runner $Runner
    $record.phase='archived'; & $Save $State
}

function Invoke-DriverStateDefaults {
    param([string]$Cli,$Defaults,$Previous,[hashtable]$Explicit=@{},[scriptblock]$Runner)
    if(-not $Runner){$Runner={param($exe,$arguments) Invoke-Native $exe $arguments}}
    $response=& $Runner $Cli @('state','snapshot')
    if($response.code -ne 0){throw "state snapshot failed: $($response.text)"}
    $snapshot=$response.text | ConvertFrom-Json -ErrorAction Stop
    if($snapshot.abi -ne 1 -or $null -eq $snapshot.error -or $snapshot.error -ne 0 -or
       $null -eq $snapshot.items -or $snapshot.items -is [string]){throw 'unsupported state snapshot protocol'}
    $now=@{}; $settings=@{}; $prior=@{}
    foreach($entry in $snapshot.items) {
        if($entry.path -ceq '' -and $entry.kind -in @('record',1)) {
            if(-not $entry.record -or $entry.record.status -notin @('ok','absent')) {throw "state record $($entry.name): unreadable"}
            if($entry.record.status -eq 'ok') {
                if($entry.name -ne 'StageHistory' -and $entry.record.type -ne 4) {throw "state record $($entry.name): wrong type"}
                if($null -ne $entry.type -and $entry.type -ne $entry.record.type) {throw "state record $($entry.name): catalog type mismatch"}
                if($entry.type -eq 4 -or $entry.record.type -eq 4) {
                    if($entry.record.type -ne 4 -or $entry.record.bytes -ne 4 -or $null -eq $entry.record.value) {throw "state record $($entry.name): malformed DWORD"}
                    $now[$entry.name]=[uint32]$entry.record.value
                }
            }
        }
    }
    foreach($p in (ConvertTo-PairList $Defaults)) {
        $matches=@($snapshot.items | Where-Object {$_.path -ceq '' -and $_.name -ceq $p.Name})
        if($matches.Count -ne 1){throw "state catalog lacks $($p.Name)"}
        $entry=$matches[0]
        # Runtime records/closures never become installer defaults.
        if($entry.kind -ne 0 -and $entry.kind -ne 'setting'){continue}
        if($entry.override.status -notin @('ok','absent')){throw "state override $($p.Name): $($entry.override.status)"}
        if($entry.override.status -eq 'ok') {
            if($entry.override.type -ne 4 -or $entry.override.bytes -ne 4 -or $null -eq $entry.override.value){throw "state override $($p.Name) is not REG_DWORD"}
            $now[$p.Name]=[uint32]$entry.override.value
        }
        $settings[$p.Name]=$p.Value
        foreach($old in (ConvertTo-PairList $Previous)){if($old.Name -ceq $p.Name){$prior[$p.Name]=$old.Value}}
    }
    foreach($name in $Explicit.Keys){if(-not $settings.ContainsKey($name)){throw "unsupported explicit setting $name"}}
    $plan=Get-RegistryDefaultPlan -Defaults $settings -Previous $prior -Current $now -Explicit $Explicit
    foreach($row in $plan) {
        if(-not $row.write){continue}
        Write-DriverStateValue -Cli $Cli -Name ([string]$row.name) -Value ([uint32]$row.value) -Runner $Runner
    }
}
