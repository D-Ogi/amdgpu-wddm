# Read-only view of the common CLI protocol. Failures stay failures; only an
# explicit absent result omits a value. No registry fallback exists here.
function Resolve-DriverStateCli([string]$Cli) {
    if($Cli){return $Cli}
    if($script:DriverStateCli){return $script:DriverStateCli}
    if($InstallRoot){return (Join-Path $InstallRoot 'tools\bc250kmd_cli.exe')}
    return (Join-Path $PSScriptRoot 'bc250kmd_cli.exe')
}
function Read-DriverStatePaths {
    param([string]$Cli)
    $Cli=Resolve-DriverStateCli $Cli
    $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try {$text=(& $Cli state paths 2>&1 | ForEach-Object {[string]$_}) -join "`n";$code=$LASTEXITCODE}
    finally {$ErrorActionPreference=$old}
    if($code -ne 0){throw "driver shared directory could not be read: $text"}
    $paths=$text | ConvertFrom-Json -ErrorAction Stop
    if($paths.abi -ne 1 -or $null -eq $paths.error -or $paths.error -ne 0 -or -not $paths.logs){throw 'invalid driver shared directory response'}
    return $paths
}
function Read-DriverStateSnapshot {
    param([string]$Cli,[scriptblock]$Runner,[int]$MaxAgeMilliseconds=0)
    $Cli=Resolve-DriverStateCli $Cli
    if($MaxAgeMilliseconds -gt 0 -and $script:DriverStateReadCache -and $script:DriverStateReadCache.cli -ceq $Cli -and
       ([datetime]::UtcNow-$script:DriverStateReadCache.utc).TotalMilliseconds -lt $MaxAgeMilliseconds) {
        return $script:DriverStateReadCache.value
    }
    if(-not $Runner) {
        $Runner={param($exe,$arguments)
            $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
            try {$text=(& $exe @arguments 2>&1 | ForEach-Object {[string]$_}) -join "`n";$code=$LASTEXITCODE}
            finally {$ErrorActionPreference=$old}
            return @{code=$code;text=$text}
        }
    }
    $response=& $Runner $Cli @('state','snapshot')
    if($response.code -ne 0){throw "shared state could not be read: $($response.text)"}
    $value=$response.text | ConvertFrom-Json -ErrorAction Stop
    if($value.abi -ne 1 -or $null -eq $value.error -or $value.error -ne 0 -or $null -eq $value.items){throw 'invalid shared state response'}
    $script:DriverStateReadCache=@{cli=$Cli;utc=[datetime]::UtcNow;value=$value}
    return $value
}
function ConvertFrom-DriverStateSnapshot {
    param($Snapshot,[string]$Path='',[switch]$Override)
    $values=@{}
    foreach($item in $Snapshot.items) {
        if($item.path -cne $Path){continue}
        if($values.ContainsKey($item.name)){throw "duplicate state item $Path/$($item.name)"}
        $read=if($item.kind -eq 'setting'){if($Override){$item.override}else{$item.effective}}else{$item.record}
        if($null -eq $read){throw "missing state reading $Path/$($item.name)"}
        if($read.status -eq 'absent'){continue}
        if($read.status -ne 'ok'){throw "state $Path/$($item.name): $($read.status), OS error $($read.os_error)"}
        $values[$item.name]=$read.value
    }
    return ,$values
}
function Read-DriverStateValues {
    param([string]$Cli,[string]$Path='',[switch]$Override,[scriptblock]$Runner,[int]$MaxAgeMilliseconds=0)
    $snapshot=Read-DriverStateSnapshot -Cli $Cli -Runner $Runner -MaxAgeMilliseconds $MaxAgeMilliseconds
    return (ConvertFrom-DriverStateSnapshot -Snapshot $snapshot -Path $Path -Override:$Override)
}
function Open-DriverStateRecordView {
    param([string]$Path,[string]$Cli,[scriptblock]$Runner)
    $values=Read-DriverStateValues -Cli $Cli -Path $Path -Runner $Runner
    if(-not $values.Count){return $null}
    $view=[pscustomobject]@{values=$values}
    $view | Add-Member ScriptMethod GetValue {param($name,$fallback)
        if($this.values.ContainsKey($name)){return $this.values[$name]};return $fallback
    }
    $view | Add-Member ScriptMethod Dispose {}
    return $view
}
function Invoke-DriverStateCommand {
    param([string]$Cli,[string[]]$Arguments,[scriptblock]$Runner)
    $Cli=Resolve-DriverStateCli $Cli
    if(-not $Runner) {
        $Runner={param($exe,$arguments)
            $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
            try {$text=(& $exe @arguments 2>&1 | ForEach-Object {[string]$_}) -join "`n";$code=$LASTEXITCODE}
            finally {$ErrorActionPreference=$old}
            @{code=$code;text=$text}
        }
    }
    $response=& $Runner $Cli $Arguments
    if($response.code -ne 0){throw "state command failed: $($response.text)"}
    $reply=$response.text | ConvertFrom-Json -ErrorAction Stop
    if($reply.abi -ne 1 -or $null -eq $reply.error -or $reply.error -ne 0){throw 'state command did not return success'}
    $script:DriverStateReadCache=$null
    return $reply
}
function Write-DriverStateValue {
    param([Parameter(Mandatory)][string]$Name,[Parameter(Mandatory)][uint32]$Value,[string]$Path='',
        [string]$Cli,[scriptblock]$Runner)
    $arguments=@('state','set',$Name,([string]$Value));if($Path){$arguments+=$Path}
    [void](Invoke-DriverStateCommand -Cli $Cli -Arguments $arguments -Runner $Runner)
    $arguments=@('state','get',$Name);if($Path){$arguments+=$Path};$arguments+='--override'
    $reply=Invoke-DriverStateCommand -Cli $Cli -Arguments $arguments -Runner $Runner
    if($reply.result.status -ne 'ok' -or $reply.result.type -ne 4 -or $reply.result.bytes -ne 4 -or
       [uint64]$reply.result.value -ne [uint64]$Value){throw "state readback differs for $Path/$Name"}
}
function Clear-DriverStateValue {
    param([Parameter(Mandatory)][string]$Name,[string]$Path='',[string]$Cli,[scriptblock]$Runner)
    $arguments=@('state','clear',$Name);if($Path){$arguments+=$Path}
    [void](Invoke-DriverStateCommand -Cli $Cli -Arguments $arguments -Runner $Runner)
    # Snapshot's top-level success is separate from this value's absence. A get
    # of an absent override has exit 3 and must not be treated as a failed store.
    $snapshot=Read-DriverStateSnapshot -Cli $Cli -Runner $Runner
    $items=@($snapshot.items | Where-Object {$_.path -ceq $Path -and $_.name -ceq $Name})
    if($items.Count -ne 1){throw "clear readback has no unique state value $Path/$Name"}
    $read=if($items[0].kind -eq 'setting'){$items[0].override}else{$items[0].record}
    if($read.status -ne 'absent'){throw "state clear readback differs for $Path/${Name}: $($read.status)"}
}
