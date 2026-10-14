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

function ConvertTo-DriverSettingsComparison($Items) {
    if($null -eq $Items -or $Items -is [string]){throw 'missing settings comparison'}
    $values=@{}
    foreach($entry in $Items) {
        if($entry.name -cnotmatch '\A[A-Za-z][A-Za-z0-9]*\z' -or $values.ContainsKey($entry.name) -or
           $entry.present -isnot [bool] -or $null -eq $entry.value -or
           [string]$entry.value -cnotmatch '\A[0-9]+\z'){throw 'invalid settings comparison entry'}
        $values[$entry.name]=[pscustomobject]@{present=$entry.present;value=[uint32]$entry.value}
    }
    if(-not $values.Count){throw 'empty settings comparison'}
    return $values
}
function Save-DriverSettingsComparison {
    param($State,[string]$Worker,[string]$Device,[string]$InfSha256,[scriptblock]$Save,[scriptblock]$Runner)
    $existing=$State.driver_settings_compare
    if($existing -and $existing.schema -eq 1 -and $existing.inf_sha256 -ieq $InfSha256 -and $existing.device -ceq $Device) {
        [void](ConvertTo-DriverSettingsComparison $existing.items);return
    }
    if($State.driver_state -and $State.driver_state.phase -eq 'prepared') {
        throw 'pending migration lacks its pre-stage settings comparison; do not replace it with a later reading'
    }
    if(-not $Runner){$Runner={param($exe,$arguments) Invoke-Native $exe $arguments}}
    $response=& $Runner $Worker @('--settings-snapshot',$Device)
    if($null -eq $response -or $response.code -ne 0){throw 'pre-stage settings read failed'}
    $snapshot=$response.text | ConvertFrom-Json -ErrorAction Stop
    if($snapshot.abi -ne 1 -or $null -eq $snapshot.error -or $snapshot.error -ne 0){throw 'invalid pre-stage settings response'}
    [void](ConvertTo-DriverSettingsComparison $snapshot.items)
    Set-StateValue $State 'driver_settings_compare' ([pscustomobject]@{schema=1;inf_sha256=$InfSha256;device=$Device;items=@($snapshot.items)})
    & $Save $State
}
function Get-DriverSettingsComparison {
    param($State,[string]$Device,[string]$InfSha256)
    $saved=$State.driver_settings_compare
    if(-not $saved -or $saved.schema -ne 1 -or $saved.device -cne $Device -or $saved.inf_sha256 -ine $InfSha256) {
        throw 'post-binding settings have no matching pre-stage comparison'
    }
    return ConvertTo-DriverSettingsComparison $saved.items
}
function Test-DriverSettingsUnchanged([hashtable]$Before,[hashtable]$Current,[string[]]$Names) {
    if($null -eq $Before){return $true}
    $unchanged=$true
    foreach($name in $Names) {
        if(-not $Before.ContainsKey($name)){throw "pre-stage comparison lacks $name"}
        $old=$Before[$name];$present=$Current.ContainsKey($name)
        if($present -ne $old.present -or ($present -and $Current[$name] -ne $old.value)){$unchanged=$false}
    }
    return $unchanged
}
function Invoke-DriverStateDefaults {
    param([string]$Cli,$Defaults,$Previous,[hashtable]$Explicit=@{},[scriptblock]$Runner,
        [hashtable]$Before=$null,[string[]]$FreshExplicit=@(),[switch]$Reopen,[switch]$FreshReopen,
        [switch]$RecordChoices)
    if(-not $Runner){$Runner={param($exe,$arguments) Invoke-Native $exe $arguments}}
    $response=& $Runner $Cli @('state','snapshot')
    if($response.code -ne 0){throw "state snapshot failed: $($response.text)"}
    $snapshot=$response.text | ConvertFrom-Json -ErrorAction Stop
    if($snapshot.abi -ne 1 -or $null -eq $snapshot.error -or $snapshot.error -ne 0 -or
       $null -eq $snapshot.items -or $snapshot.items -is [string]){throw 'unsupported state snapshot protocol'}
    $now=@{}; $settings=@{}; $prior=@{}
    foreach($entry in $snapshot.items) {
        # Choice records can depend on settings not owned by the release table.
        if($entry.path -ceq '' -and $entry.kind -in @('setting',0) -and
           ($entry.type -eq 4 -or $entry.override.type -eq 4)) {
            if($entry.override.status -notin @('ok','absent')){throw "state override $($entry.name): unreadable"}
            if($entry.override.status -eq 'ok') {
                if($entry.override.type -ne 4 -or $entry.override.bytes -ne 4 -or $null -eq $entry.override.value){throw "state override $($entry.name): malformed DWORD"}
                $now[$entry.name]=[uint32]$entry.override.value
            }
        }
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
    if($null -ne $Before) {
        foreach($name in $settings.Keys){if(-not $Before.ContainsKey($name)){throw "pre-stage comparison lacks $name"}}
    }
    if($RecordChoices -and $Explicit.ContainsKey('CuMode')) {
        $choice=@($snapshot.items | Where-Object {$_.path -ceq '' -and $_.name -ceq 'CuModeChosen'})
        if($choice.Count -ne 1 -or $choice[0].kind -notin @('record',1) -or $choice[0].type -ne 4){throw 'CuModeChosen is not a DWORD record in the state catalog'}
    }
    $plan=Get-RegistryDefaultPlan -Defaults $settings -Previous $prior -Current $now -Explicit $Explicit -Reopen:$Reopen
    # Check all comparisons before the first write, including records that a
    # repair clears and the mask/choice pair which describes an explicit CU mode.
    foreach($row in $plan) {
        if(-not $row.write){continue}
        # A remembered command is not a new request. Absence is a choice too.
        # The observation supplies only equality, never the replacement value.
        $names=@($row.name)
        if($row.PSObject.Properties['clear']){$names+=@($row.clear)}
        if($RecordChoices -and $row.name -ceq 'CuMode'){$names+=@('CuModeChosen','CuDisableWgp')}
        $same=Test-DriverSettingsUnchanged -Before $Before -Current $now -Names $names
        if(-not $same -and $FreshExplicit -cnotcontains $row.name -and -not ($FreshReopen -and $row.decision -eq 'reopened')) {
            $row.write=$false
        }
    }
    foreach($row in $plan) {
        if(-not $row.write){continue}
        Write-DriverStateValue -Cli $Cli -Name ([string]$row.name) -Value ([uint32]$row.value) -Runner $Runner
        if($row.PSObject.Properties['clear']) {
            foreach($name in @($row.clear)){Clear-DriverStateValue -Cli $Cli -Name ([string]$name) -Runner $Runner}
        }
        if($RecordChoices -and $row.name -ceq 'CuMode' -and $row.decision -eq 'command') {
            # CU-map contract: the installed mask is retained; the record stores
            # the complete request (mode | mask << 8), not just the mode number.
            $mask=if($now.ContainsKey('CuDisableWgp')){[uint32]$now.CuDisableWgp}else{0}
            $chosen=[uint32](([uint32]$row.value -band 0xff) -bor ($mask -shl 8))
            Write-DriverStateValue -Cli $Cli -Name CuModeChosen -Value $chosen -Record -Runner $Runner
        }
    }
}
