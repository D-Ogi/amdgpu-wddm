# State schema 2 installer transaction. PowerShell owns orchestration only; the
# bounded SYSTEM worker owns capture, freeze, protected files and acknowledgment.
function Invoke-DriverStateWorker {
    param([string]$Worker,[ValidateSet('prepare','ack','archive','abandon')][string]$Operation,
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

# The protected state directory of the worker (driver/state/windows_handoff.c). Administrators may read it.
function Get-DriverStateDirectory {
    $data=if($env:DriverData){$env:DriverData}else{Join-Path $env:SystemRoot 'System32\drivers\DriverData'}
    return (Join-Path $data 'amdgpu-wddm\StateV2')
}

# BD-262: when the prepare of an install finds the removal record of a version whose state catalog this worker
# cannot read (a lab or pre-release build), it moves that record aside as retired-<transaction>-<name> and publishes
# a handoff without settings (origin 1, fresh). The record of an earlier release that the worker reads by setting
# name gives origin 4. Reads only; returns the names of the records moved aside, or nothing.
function Get-DriverStateRetiredRecords {
    param([string]$Transaction,[string]$Directory=(Get-DriverStateDirectory))
    $retired=@(Get-ChildItem -LiteralPath $Directory -Filter "retired-$Transaction-removed*.bin" -File -ErrorAction SilentlyContinue)
    if(-not $retired.Count) { return @() }
    try { $b=[IO.File]::ReadAllBytes((Join-Path $Directory 'pending.bin')) } catch { return @() }
    if($b.Length -lt 360 -or [BitConverter]::ToUInt32($b,0) -ne 0x32545341 -or [BitConverter]::ToUInt32($b,16) -ne 1) { return @() }
    return @($retired | ForEach-Object { $_.Name })
}

# BD-298: the installer record of the state transaction (state.json driver_state) is a state machine with explicit
# phases. No record or the phase 'acknowledged' or 'abandoned' has no open transaction. 'prepared' has one: the worker
# can hold its handoff (pending.bin) and its freeze of the installed store.
#   (none), acknowledged, abandoned --install--> prepared (a new transaction)
#   prepared --install of the same package--> prepared (the same transaction: the prepare is repeated)
#   prepared --acknowledgment of the same package--> acknowledged
#   prepared --install of another package, or uninstall--> abandoned (the worker step 'abandon', then a new transaction
#                                                         or the removal)
# 'abandoned' is terminal for its transaction: no step resumes or acknowledges it. The table gives the step for each
# phase and intent. exact: the record names this product, device and INF ($null: either).
$script:DriverStateSteps=@(
    @{phase='none';intent='install';exact=$null;step='begin'}
    @{phase='none';intent='complete';exact=$null;step='refuse'}
    @{phase='none';intent='remove';exact=$null;step='proceed'}
    @{phase='prepared';intent='install';exact=$true;step='resume'}
    @{phase='prepared';intent='install';exact=$false;step='abandon'}
    @{phase='prepared';intent='complete';exact=$true;step='acknowledge'}
    @{phase='prepared';intent='complete';exact=$false;step='refuse'}
    @{phase='prepared';intent='remove';exact=$null;step='abandon'}
    @{phase='acknowledged';intent='install';exact=$null;step='begin'}
    @{phase='acknowledged';intent='complete';exact=$true;step='acknowledge'}
    @{phase='acknowledged';intent='complete';exact=$false;step='refuse'}
    @{phase='acknowledged';intent='remove';exact=$null;step='proceed'}
    @{phase='abandoned';intent='install';exact=$null;step='begin'}
    @{phase='abandoned';intent='complete';exact=$null;step='refuse'}
    @{phase='abandoned';intent='remove';exact=$null;step='proceed'}
)
function Get-DriverStateStep {
    param($Record,[ValidateSet('install','complete','remove')][string]$Intent,[string]$Product,[string]$Device,[string]$InfSha256)
    $phase='none'
    if($Record) {
        if($Record.schema -ne 2 -or [string]$Record.transaction -cnotmatch '\A[0-9a-f]{32}\z') { throw 'invalid driver state transaction record' }
        $phase=[string]$Record.phase
    }
    $exact=[bool]($Record -and $Record.product -ceq $Product -and $Record.device -ceq $Device -and $Record.inf_sha256 -ieq $InfSha256)
    foreach($row in $script:DriverStateSteps) {
        if($row.phase -ceq $phase -and $row.intent -ceq $Intent -and ($null -eq $row.exact -or $row.exact -eq $exact)) { return $row.step }
    }
    throw 'invalid driver state transaction phase'
}

# The step 'abandon': the SYSTEM worker moves the handoff of the stopped install aside as
# retired-<transaction>-pending.bin and releases the freeze of that transaction. Then the record is 'abandoned', and a
# copy goes to driver_state_history. The record and the retired file stay as evidence. A retry after any failure
# repeats the worker step: that step does only what remains.
function Stop-DriverStateTransaction {
    param($State,[string]$Worker,[ValidateSet('install','remove')][string]$Reason,[string]$Product,
        [scriptblock]$Save,[scriptblock]$Runner,[string]$StateDirectory=(Get-DriverStateDirectory),
        [string]$StoreKey='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\SharedState\StateV2')
    $record=$State.driver_state
    if(-not $record -or $record.phase -cne 'prepared') { throw 'no prepared driver state transaction to abandon' }
    $old=[string]$record.product
    # Administrators may read the state directory: the log says what the worker found and left.
    $pendingPath=Join-Path $StateDirectory 'pending.bin';$retiredPath=Join-Path $StateDirectory "retired-$($record.transaction)-pending.bin"
    $hadPending=Test-Path -LiteralPath $pendingPath -PathType Leaf;$hadRetired=Test-Path -LiteralPath $retiredPath -PathType Leaf
    # A store whose first transaction did not finish (StateV2 with no Active): only the file moves, and the store stays.
    $unfinished=$false
    try { if(Test-Path -LiteralPath $StoreKey){ $unfinished=$null -eq (Get-Item -LiteralPath $StoreKey).GetValue('Active') } } catch { }
    try {
        Invoke-DriverStateWorker -Worker $Worker -Operation abandon -Transaction $record.transaction -Product $old -Device $record.device -Runner $Runner
    } catch {
        $text=$_.Exception.Message
        $why=switch -Regex ($text) {
            '\(exit 13\)'   { 'its state file is damaged or of an unknown form. The file was kept.'; break }
            '\(exit 1306\)' { 'the state files belong to another install. They were kept.'; break }
            '\(exit 183\)'  { 'a retired file of an earlier attempt has other content. Both files were kept.'; break }
            default         { 'the state worker failed.' }
        }
        throw "The stopped install of driver version $old cannot be abandoned: $why Directory: $StateDirectory. Detail: $text"
    }
    $record.phase='abandoned'
    $record | Add-Member -NotePropertyName abandoned_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $record | Add-Member -NotePropertyName abandoned_for -NotePropertyValue $(if($Reason -eq 'remove'){'uninstall'}else{"install of $Product"}) -Force
    $history=@(@($State.driver_state_history) | Where-Object { $_ }) + @(($record | ConvertTo-Json -Depth 4 -Compress | ConvertFrom-Json))
    Set-StateValue $State 'driver_state_history' @($history | Select-Object -Last 8)
    & $Save $State
    Write-Warn2 ("The install of driver version $old did not finish (for example after a power loss or a blocked program). " +
        "It was abandoned. Its saved settings record was kept as evidence.")
    Write-Log "   state: transaction $($record.transaction) abandoned; its handoff, if any, is retired-$($record.transaction)-pending.bin in $StateDirectory"
    if($hadPending -and $hadRetired) {
        Write-Log "   state: pending.bin was identical to the retired copy of an earlier abandon of this transaction; the worker removed pending.bin after a byte compare"
    }
    if($unfinished) {
        Write-Log "   state: the driver had started this install and stopped before it completed (StateV2 with no Active); the store stays, and the next transaction reads the legacy settings"
    }
    if(Test-Path -LiteralPath $retiredPath -PathType Leaf) {
        try { Write-Log "   state: $retiredPath SHA256 $((Get-FileHash -LiteralPath $retiredPath -Algorithm SHA256).Hash)" } catch { }
    }
}

# Before the install of a package: abandons a prepared transaction of another package. Returns the step of the table.
function Resolve-DriverStateTransaction {
    param($State,[string]$Worker,[string]$Product,[string]$Device,[string]$InfSha256,
        [scriptblock]$Save,[scriptblock]$Runner,[string]$StateDirectory=(Get-DriverStateDirectory))
    $step=Get-DriverStateStep -Record $State.driver_state -Intent install -Product $Product -Device $Device -InfSha256 $InfSha256
    if($step -eq 'abandon') {
        Stop-DriverStateTransaction -State $State -Worker $Worker -Reason install -Product $Product -Save $Save -Runner $Runner -StateDirectory $StateDirectory
        $step=Get-DriverStateStep -Record $State.driver_state -Intent install -Product $Product -Device $Device -InfSha256 $InfSha256
    }
    return $step
}

function Start-DriverStateTransaction {
    param($State,[string]$Worker,[string]$Product,[string]$Device,[string]$InfSha256,
        [scriptblock]$Save,[scriptblock]$Runner,[string]$StateDirectory=(Get-DriverStateDirectory))
    $step=Resolve-DriverStateTransaction -State $State -Worker $Worker -Product $Product -Device $Device -InfSha256 $InfSha256 `
        -Save $Save -Runner $Runner -StateDirectory $StateDirectory
    if($step -eq 'begin') {
        $record=[pscustomobject]@{schema=2;transaction=[guid]::NewGuid().ToString('N');phase='prepared';
            product=$Product;device=$Device;inf_sha256=$InfSha256}
        Set-StateValue $State 'driver_state' $record
        # Identity reaches durable installer state before the worker can freeze.
        & $Save $State
    } elseif($step -ne 'resume') { throw "unexpected driver state step $step" }
    $record=$State.driver_state
    Invoke-DriverStateWorker -Worker $Worker -Operation prepare -Transaction $record.transaction -Product $Product -Device $Device -Runner $Runner
    $retired=@(Get-DriverStateRetiredRecords -Transaction $record.transaction -Directory $StateDirectory)
    if($retired.Count) {
        # BD-298 follow-up: a legacy removal record (origin 2, header offset 16) is the archive of a driver whose first
        # install did not finish: its settings never became active.
        $legacy=@($retired | Where-Object {
            try { $b=[IO.File]::ReadAllBytes((Join-Path $StateDirectory $_)); $b.Length -ge 360 -and [BitConverter]::ToUInt32($b,16) -eq 2 } catch { $false } })
        if($legacy.Count -eq $retired.Count) {
            Write-Warn2 ('The settings of the driver version that was removed before this install are not carried over: ' +
                'the install of that version did not finish, so its settings never became active. The driver starts with its default settings. ' +
                'Set your choices again in the control app.')
        } else {
            Write-Warn2 ('The saved settings of the driver version that was removed before this install cannot be carried over: ' +
                'that version saved them in a form that this version cannot read. The driver starts with its default settings. ' +
                'Set your choices again in the control app.')
        }
        Write-Log "   state: the removal record was kept as $($retired -join ', ') in $StateDirectory"
    }
    return $record
}

function Complete-DriverStateTransaction {
    param($State,[string]$Worker,[string]$Product,[string]$Device,[string]$InfSha256,
        [scriptblock]$Save,[scriptblock]$Runner)
    $record=$State.driver_state
    $step='refuse'
    try { $step=Get-DriverStateStep -Record $record -Intent complete -Product $Product -Device $Device -InfSha256 $InfSha256 } catch { }
    if($step -ne 'acknowledge') { throw 'driver binding has no matching prepared state transaction' }
    # The native operation checks Active/Ready/Catalog with the common adapter,
    # archives first, and removes only the matching pending file by its handle.
    Invoke-DriverStateWorker -Worker $Worker -Operation ack -Transaction $record.transaction -Product $Product -Device $Device -Runner $Runner
    $record.phase='acknowledged'; & $Save $State
}

function Save-DriverStateBeforeRemoval {
    param($State,[string]$Worker,[string]$Product,[string]$Device,[scriptblock]$Save,[scriptblock]$Runner,
        [string]$StateDirectory=(Get-DriverStateDirectory),[string]$AbandonWorker='')
    # BD-298: a stopped install (prepared) is abandoned first. Its settings stay in the store, and the archive below
    # captures them for a later install. The worker of an installed release before BD-298 has no abandon step: the
    # uninstaller of a newer package gives its own worker as -AbandonWorker.
    if(-not $AbandonWorker){$AbandonWorker=$Worker}
    $step=Get-DriverStateStep -Record $State.driver_state -Intent remove
    if($step -eq 'abandon') {
        Stop-DriverStateTransaction -State $State -Worker $AbandonWorker -Reason remove -Save $Save -Runner $Runner -StateDirectory $StateDirectory
    } elseif($step -ne 'proceed') { throw "unexpected driver state step $step" }
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
    # Check all comparisons before the first write, including the closure
    # records and the mask/choice pair which describes an explicit CU mode.
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
        # The legacy registry plan also carries closure-record deletion hints.
        # DpmClosedReason is read-only to users. For every closure, writing the
        # reopened setting requests recovery, and KMD initialization clears its
        # reason. Do not require user-side deletion of these driver records.
        # Retain those records here, including when another setting write fails.
        if($RecordChoices -and $row.name -ceq 'CuMode' -and $row.decision -eq 'command') {
            # CU-map contract: the installed mask is retained; the record stores
            # the complete request (mode | mask << 8), not just the mode number.
            $mask=if($now.ContainsKey('CuDisableWgp')){[uint32]$now.CuDisableWgp}else{0}
            $chosen=[uint32](([uint32]$row.value -band 0xff) -bor ($mask -shl 8))
            Write-DriverStateValue -Cli $Cli -Name CuModeChosen -Value $chosen -Record -Runner $Runner
        }
    }
}
