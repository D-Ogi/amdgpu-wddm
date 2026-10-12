# Automatic diagnostics of a driver start that did not pass (owner, 2026-10-12: the installer collects the logs
# itself when the install does not pass). Until now a tester had to be asked for every reading by hand, one mail at a
# time; the field report 20261011T222253Z is the case this file was written for: the device stood at problem code 43
# (CM_PROB_FAILED_POST_START) with the start record of that boot already written, and the support file carried the
# one-line summary of verify and nothing else.
#
# The installer's verify step calls Get-StartFailureDecision with the readings it already took. When the decision
# says the driver did not start, Write-StartFailureDiagnostics writes ONE text file for that run, next to the
# installer's log, and the engine names it in result.json. The setup window puts it into the support file as
# run-<n>-diagnostics.txt; the console route prints its path in its last lines.
#
# What goes into the file:
#   1. the decision and the readings it was made from;
#   2. StartRecord and, when present, StopRecord of the bc250kmd service. The stop record (KMD 0.7.216.39 and later)
#      is written when Windows stops a device whose start nothing had confirmed, and a start that IS confirmed
#      writes no stop record at all when Windows stops it later. A start that FAILED writes none either: the driver
#      calls its own stop to unwind such a start, Windows stopped nothing there, and StartRecord above names the
#      step and the status that failed. An absent stop record is therefore normal, and a
#      stop record that is there may be from an earlier boot: its BootId and its time are compared with this boot
#      before the file names it as this boot's, and the file says where this boot's number was read (the driver
#      writes KUSER_SHARED_DATA.BootId, whose registry copy can lag a boot by one: BD-130). Its Status value is
#      always 0 and is never read as a success:
#      StopDevice gives the driver no code for why Windows stopped the device;
#   3. the GPU device: problem code with its name, status, driver version, INF, service;
#   4. the event logs of THIS boot from Display, Microsoft-Windows-Kernel-PnP and the Service Control Manager in the
#      System log and from Microsoft-Windows-DxgKrnl in its own two channels: errors and warnings, and DxgKrnl's
#      information entries as well, bounded in count;
#   5. the setupapi.dev.log sections whose header names the GPU's hardware id, the newest ones, bounded;
#   6. the files of C:\BC250\kmdlog when that folder exists, newest first, bounded.
#
# Bounds and the rule that keeps this harmless: the whole collection has 20 s and 4 MiB. A section that passes the
# budget, is cut by the size bound or fails altogether writes that fact into the file as a line of its own. Nothing
# here changes a verify result, a message id or an exit code: verify decides as it always did, and this file is
# detail for a problem report. Windows PowerShell 5.1 syntax only. Dot-sourced after start-confirm-core.ps1, whose
# readers of the two records it uses.

$script:DiagnosticsSchema = 'amdgpu-wddm.diagnostics/1'
$script:DiagnosticsMaxBytes = 4 * 1024 * 1024
$script:DiagnosticsBudgetSeconds = 20
# The event-log sources this failure can speak through: each one names the LOG as well as the provider, because a
# provider only answers in the logs it writes to, and a query that names another log throws on every call. DxgKrnl
# has no System channel at all (its channels are Microsoft-Windows-DxgKrnl-Admin and -Operational, and its other
# links are trace channels), so the row that was meant to carry the GPU's own voice read as "not read" on every PC
# until this list named those two logs. The levels: 1 critical, 2 error, 3 warning, 4 information - DxgKrnl says
# what it did to the adapter in information entries, the others are only worth reading when they complain.
# The service's own word about a start that did not happen comes from the Service Control Manager in the System log
# ("the bc250kmd service failed to start", "boot-start driver did not load"): the KMD writes no event-log entry of
# its own today (no IoAllocateErrorLogEntry, IoWriteErrorLogEntry or EventWrite anywhere in driver/kmd), so a
# bc250kmd provider row could only ever answer "there is no such provider on this computer".
# test-diagnostics.ps1 holds every row of this list against Get-WinEvent -ListProvider of this computer, so a log a
# provider does not write to cannot come back unnoticed.
$script:DiagnosticsEventSources = @(
    [ordered]@{ log = 'System';                                levels = @(1, 2, 3);    provider = 'Display' }
    [ordered]@{ log = 'Microsoft-Windows-DxgKrnl-Admin';        levels = @(1, 2, 3, 4); provider = 'Microsoft-Windows-DxgKrnl' }
    [ordered]@{ log = 'Microsoft-Windows-DxgKrnl-Operational';  levels = @(1, 2, 3, 4); provider = 'Microsoft-Windows-DxgKrnl' }
    [ordered]@{ log = 'System';                                levels = @(1, 2, 3);    provider = 'Microsoft-Windows-Kernel-PnP' }
    [ordered]@{ log = 'System';                                levels = @(1, 2, 3);    provider = 'Service Control Manager' }
)
$script:DiagnosticsMaxEventsPerProvider = 60
$script:DiagnosticsMaxSetupApiSections = 5
$script:DiagnosticsMaxSetupApiLines = 400
$script:DiagnosticsMaxKmdLogFiles = 3
$script:DiagnosticsMaxKmdLogLines = 400

# The configuration manager's problem codes (cfgmgr32.h CM_PROB_*), so that the file names the code a tester reads
# in Device Manager instead of printing a bare number. A code without a line here prints its number alone.
$script:DiagnosticsProblemNames = @{
    0 = 'the device is working properly'; 1 = 'CM_PROB_NOT_CONFIGURED'; 2 = 'CM_PROB_DEVLOADER_FAILED'
    3 = 'CM_PROB_OUT_OF_MEMORY'; 4 = 'CM_PROB_ENTRY_IS_WRONG_TYPE'; 5 = 'CM_PROB_LACKED_ARBITRATOR'
    6 = 'CM_PROB_BOOT_CONFIG_CONFLICT'; 7 = 'CM_PROB_FAILED_FILTER'; 8 = 'CM_PROB_DEVLOADER_NOT_FOUND'
    9 = 'CM_PROB_INVALID_DATA'; 10 = 'CM_PROB_FAILED_START'; 11 = 'CM_PROB_LIAR'; 12 = 'CM_PROB_NORMAL_CONFLICT'
    13 = 'CM_PROB_NOT_VERIFIED'; 14 = 'CM_PROB_NEED_RESTART'; 15 = 'CM_PROB_REENUMERATION'
    16 = 'CM_PROB_PARTIAL_LOG_CONF'; 17 = 'CM_PROB_UNKNOWN_RESOURCE'; 18 = 'CM_PROB_REINSTALL'
    19 = 'CM_PROB_REGISTRY'; 20 = 'CM_PROB_VXDLDR'; 21 = 'CM_PROB_WILL_BE_REMOVED'; 22 = 'CM_PROB_DISABLED'
    23 = 'CM_PROB_DEVLOADER_NOT_READY'; 24 = 'CM_PROB_DEVICE_NOT_THERE'; 25 = 'CM_PROB_MOVED'
    26 = 'CM_PROB_TOO_EARLY'; 27 = 'CM_PROB_NO_VALID_LOG_CONF'; 28 = 'CM_PROB_FAILED_INSTALL'
    29 = 'CM_PROB_HARDWARE_DISABLED'; 30 = 'CM_PROB_CANT_SHARE_IRQ'; 31 = 'CM_PROB_FAILED_ADD'
    32 = 'CM_PROB_DISABLED_SERVICE'; 33 = 'CM_PROB_TRANSLATION_FAILED'; 34 = 'CM_PROB_NO_SOFTCONFIG'
    35 = 'CM_PROB_BIOS_TABLE'; 36 = 'CM_PROB_IRQ_TRANSLATION_FAILED'; 37 = 'CM_PROB_FAILED_DRIVER_ENTRY'
    38 = 'CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD'; 39 = 'CM_PROB_DRIVER_FAILED_LOAD'
    40 = 'CM_PROB_DRIVER_SERVICE_KEY_INVALID'; 41 = 'CM_PROB_LEGACY_SERVICE_NO_DEVICES'
    42 = 'CM_PROB_DUPLICATE_DEVICE'; 43 = 'CM_PROB_FAILED_POST_START'; 44 = 'CM_PROB_HALTED'
    45 = 'CM_PROB_PHANTOM'; 46 = 'CM_PROB_SYSTEM_SHUTDOWN'; 47 = 'CM_PROB_HELD_FOR_EJECT'
    48 = 'CM_PROB_DRIVER_BLOCKED'; 49 = 'CM_PROB_REGISTRY_TOO_LARGE'; 50 = 'CM_PROB_SETPROPERTIES_FAILED'
    51 = 'CM_PROB_WAITING_ON_DEPENDENCY'; 52 = 'CM_PROB_UNSIGNED_DRIVER'; 53 = 'CM_PROB_USED_BY_DEBUGGER'
    54 = 'CM_PROB_DEVICE_RESET'; 55 = 'CM_PROB_CONSOLE_LOCKED'; 56 = 'CM_PROB_NEED_CLASS_CONFIG'
    57 = 'CM_PROB_GUEST_ASSIGNMENT_FAILED'
}

# The problem code as a number, whatever shape the reading has: Win32_PnPEntity ConfigManagerErrorCode comes back
# as a number on some builds and as its CM_PROB_* name on others. The field report 20261011T222253Z carries the
# name ("problem code CM_PROB_FAILED_POST_START"), so a reader that only casts to [int] learns nothing from the
# one case this file exists for. $null when neither shape can be read.
function ConvertTo-DiagnosticsProblemCode {
    param([AllowNull()]$Code)
    if ($null -eq $Code -or [string]$Code -eq '') { return $null }
    $n = 0
    if ([int]::TryParse([string]$Code, [ref]$n)) { return $n }
    $name = ([string]$Code).Trim()
    foreach ($k in @($script:DiagnosticsProblemNames.Keys)) { if ($script:DiagnosticsProblemNames[$k] -eq $name) { return [int]$k } }
    return $null
}
# "43 (CM_PROB_FAILED_POST_START)", or what was read when the table does not know it, or "unreadable".
function Format-DiagnosticsProblemCode {
    param([AllowNull()]$Code)
    if ($null -eq $Code -or [string]$Code -eq '') { return 'unreadable' }
    $n = ConvertTo-DiagnosticsProblemCode $Code
    if ($null -eq $n) { return "$([string]$Code) (not a code this installer knows)" }
    $name = $script:DiagnosticsProblemNames[$n]
    if ($name) { return "$n ($name)" }
    return [string]$n
}

# Pure: did this verify reading find a driver that did not start? Every input is what verify already read, and every
# one of them may be $null for "could not be read":
#   $ProblemCode     the device's ConfigManagerErrorCode (0 = working properly)
#   $DevicePresent   $false when no device with our hardware id is there at all
#   $DeviceStatus    the PnP status ('OK', 'Error', 'Unknown', ...)
#   $Record          the KMD's StartRecord (Read-StartRecordFromKey) or $null for an absent one
#   $BootUtc         this boot's start time (Get-StartRecordBootUtc), which decides whether the record is this boot's
#   $HealthFlags     the KMD start-health flags, $null when there was no reading at all
#   $LastStage       the KMD's LastStage, 50 at the first mode set of a healthy start
# Returns collect (true or false), the reasons in plain words, and a one-line summary for the log. The hard signals
# below each mean the start failed on their own. An absent start record is NOT one of them: a KMD before 0.7.216.30
# keeps none, and a healthy start of such a driver must not fetch a diagnostics file at every verify.
function Get-StartFailureDecision {
    param([AllowNull()]$ProblemCode, [AllowNull()]$DevicePresent, [AllowNull()][string]$DeviceStatus,
          [AllowNull()]$Record, [AllowNull()]$BootUtc, [AllowNull()]$HealthFlags, [AllowNull()]$LastStage)
    $reasons = New-Object Collections.ArrayList
    $soft = New-Object Collections.ArrayList
    if ($null -ne $DevicePresent -and -not [bool]$DevicePresent) {
        [void]$reasons.Add('no graphics device with the BC-250 hardware id is present')
    } else {
        $code = ConvertTo-DiagnosticsProblemCode $ProblemCode
        if ($null -eq $code) { [void]$reasons.Add("the device problem code cannot be read ($(Format-DiagnosticsProblemCode $ProblemCode))") }
        elseif ($code -ne 0) { [void]$reasons.Add("the device reports problem code $(Format-DiagnosticsProblemCode $code)") }
        if ($DeviceStatus -and $DeviceStatus -ne 'OK') { [void]$reasons.Add("the device status is $DeviceStatus") }
    }
    # The driver's own reason, when it is this boot's (the reader's rule of start-confirm-core.ps1).
    if ($null -eq $Record) {
        [void]$soft.Add('the driver kept no start record (a driver before 0.7.216.30, or the key cannot be read)')
    } elseif ($null -eq $BootUtc) {
        # The boot time of this computer could not be read at all (Get-StartRecordBootUtc gives $null when no CIM
        # answers). No record can be placed in this boot then, so the record's own word is a note here and never a
        # reason: a record that a failed start of an EARLIER boot left would otherwise make a healthy install
        # collect a file and name that boot's step as the reason, with no note to say which boot it was from.
        # The hard signals of the device, the start health and the stage above are unaffected: a start that failed
        # in THIS boot is still read as a failure, from the readings that do not need a boot time.
        [void]$soft.Add("the boot time of this computer is unknown, so the start record of $([string]$Record.TimeUtc) (result '$([string]$Record.Result)') cannot be placed in this boot and decides nothing")
    } elseif (Test-StartRecordFromEarlierBoot -Record $Record -BootUtc $BootUtc) {
        [void]$soft.Add("the start record was written before this boot ($([string]$Record.TimeUtc)), so it is not this start's")
    } else {
        switch ([string]$Record.Result) {
            'failed'  { [void]$reasons.Add("the start record of this boot says the start failed in $(if ($Record.Step -and $Record.Step -ne '-') { [string]$Record.Step } else { 'a step it does not name' })") }
            'guard'   { [void]$reasons.Add("the start record of this boot says the driver's own boot-loop guard refused the start") }
            'attempt' { [void]$reasons.Add('the start record of this boot says a start neither failed nor finished') }
            'started' { [void]$soft.Add('the start record of this boot says the start reached a started device') }
            default   { [void]$reasons.Add("the start record of this boot is unreadable (result '$([string]$Record.Result)')") }
        }
    }
    if ($null -eq $HealthFlags) { [void]$reasons.Add('the driver answered no start-health reading at all') }
    else {
        $f = 0
        try { $f = [int]$HealthFlags } catch { }
        if (($f -band 7) -ne 7) { [void]$reasons.Add("the start is not full, ready and visible (start-health flags $f)") }
        elseif (($f -band 8) -eq 0) { [void]$soft.Add("the start is healthy but nothing has confirmed it yet (start-health flags $f)") }
    }
    $stage = $null
    try { if ($null -ne $LastStage -and [string]$LastStage -ne '') { $stage = [int]$LastStage } } catch { }
    if ($null -eq $LastStage) { [void]$soft.Add('the driver answered no LastStage') }
    elseif ($null -eq $stage) { [void]$soft.Add("the driver's LastStage is unreadable ('$([string]$LastStage)')") }
    elseif ($stage -lt 50) { [void]$reasons.Add("the start stopped at stage $stage, before the first mode set (50)") }
    elseif ($stage -gt 50) { [void]$reasons.Add("the device left the started stages (LastStage $stage)") }
    $collect = [bool]$reasons.Count
    $all = @(@($reasons) + @($soft))
    return [ordered]@{
        collect = $collect
        reasons = @($reasons)
        notes = @($soft)
        summary = $(if ($collect) { 'start failure: ' + (@($reasons) -join '; ') } else { 'no start failure: ' + $(if ($all.Count) { $all -join '; ' } else { 'every reading is as a started driver gives it' }) })
    }
}

# ---- the bounded sink -------------------------------------------------------------------------------------------
# One text under a byte bound. Every line of the file goes through Add-DiagnosticsLine, so the 4 MiB bound holds
# whatever a section hands it, and the file says where it was cut instead of ending in the middle of a line.
function New-DiagnosticsSink {
    param([int]$MaxBytes = $script:DiagnosticsMaxBytes)
    return [ordered]@{ text = (New-Object Text.StringBuilder); max = $MaxBytes; bytes = 0; cut = $false; cut_at = $null }
}
function Add-DiagnosticsLine {
    param([Parameter(Mandatory)]$Sink, [AllowEmptyString()][AllowNull()][string]$Text, [string]$Where = '')
    $line = [string]$Text
    if ($null -eq $line) { $line = '' }
    # UTF-8 bytes and not characters, because that is what the file is written in (WriteAllText with UTF8Encoding
    # below) and what the bound promises. Everything in this file comes from the OS in the OS's own language:
    # localised event messages, setupapi.dev.log text and the driver's log lines, and on a ja-JP or ko-KR
    # installation (the lab's OS matrix has both) one character costs three bytes. Counting characters let a file
    # that reported 4096 KiB reach about three times that on disk.
    $n = [Text.Encoding]::UTF8.GetByteCount($line) + 2
    if ($Sink.cut) { return $false }
    if ($Sink.bytes + $n -gt $Sink.max) {
        $Sink.cut = $true
        $Sink.cut_at = $Where
        $note = "[the $([int]($Sink.max / 1024)) KiB bound was reached$(if ($Where) { " in '$Where'" }): nothing after this point was collected]"
        [void]$Sink.text.AppendLine($note)
        $Sink.bytes = $Sink.bytes + [Text.Encoding]::UTF8.GetByteCount($note) + 2
        return $false
    }
    [void]$Sink.text.AppendLine($line)
    $Sink.bytes = $Sink.bytes + $n
    return $true
}
function Add-DiagnosticsLines {
    param([Parameter(Mandatory)]$Sink, [AllowEmptyCollection()][AllowNull()]$Lines, [string]$Where = '')
    foreach ($l in @($Lines)) { if (-not (Add-DiagnosticsLine -Sink $Sink -Text ([string]$l) -Where $Where)) { return $false } }
    return $true
}

# ---- the readers ------------------------------------------------------------------------------------------------
# One record of the service's Parameters key as lines: the summary line, the values of the record and its kept ring
# lines. $SubKey is 'StartRecord' or 'StopRecord', and the two are read by their own readers, because their Result
# values mean different things and the stop record carries seven values the start record does not have
# (start-confirm-core.ps1, THE READER'S RULE and THE STOP READER'S RULE). A stop record also needs this boot's
# BootId: a stop that a confirmed start preceded writes nothing, so a record of an older boot survives a healthy
# install and must be named as that boot's and not as this one's.
function Get-DiagnosticsRecordLines {
    param([Parameter(Mandatory)]$Root, [Parameter(Mandatory)][string]$ParametersSubKey, [Parameter(Mandatory)][string]$SubKey,
          [AllowNull()]$BootUtc, [AllowNull()]$BootId, [AllowNull()][string]$BootIdSource, [switch]$StartRecord)
    $k = $Root.OpenSubKey("$ParametersSubKey\$SubKey")
    if (-not $k) { return @("$SubKey : absent") }
    try {
        $lines = New-Object Collections.ArrayList
        if ($StartRecord) {
            $r = Read-StartRecordFromKey $k -AllowNoResult
            if ($null -eq $r) { return @("$SubKey : the key is there and holds no value this reader knows") }
            [void]$lines.Add("$SubKey : " + (Format-StartRecord -Record $r -BootUtc $BootUtc))
        } else {
            $r = Read-StopRecordFromKey $k
            if ($null -eq $r) { return @("$SubKey : the key is there and holds no value this reader knows") }
            [void]$lines.Add("$SubKey : " + (Format-StopRecord -Record $r -BootUtc $BootUtc -BootId $BootId))
            # The source of this boot's number belongs next to the comparison: the registry copy of the counter can
            # lag this boot by one, so a record of this boot reads as another boot's (BD-130).
            [void]$lines.Add("$SubKey boot: this record $(Test-StopRecordBoot -Record $r -BootUtc $BootUtc -BootId $BootId); the record's BootId $(if ($null -eq $r.BootId) { 'is absent' } else { "is $($r.BootId)" }), this boot's $(if ($null -eq $BootId) { 'cannot be read' } else { "is $BootId" })$(if ($BootIdSource) { ", read from $BootIdSource" } else { '' })")
            [void]$lines.Add("$SubKey values: Result '$([string]$r.Result)', Step '$([string]$r.Step)', SubStep '$([string]$r.SubStep)', Stage $($r.Stage), StartStage $($r.StartStage), HealthFlags $($r.HealthFlags), BootId $($r.BootId), Generation '$([string]$r.Generation)', Epoch '$([string]$r.Epoch)', TimeUtc '$([string]$r.TimeUtc)', StopTimeUtc '$([string]$r.StopTimeUtc)', LogLines $($r.LogLines), Version $(if ($null -ne $r.Version) { '0x{0:X8}' -f ([uint32]$r.Version) } else { 'unknown' })")
            [void]$lines.Add("$SubKey status: $(Format-StartRecordStatus $r.Status), which is no verdict: StopDevice gives the driver no code for why Windows stopped the device, so this value is 0 whatever the cause was")
        }
        if ($r.Detail) { [void]$lines.Add("$SubKey detail: $([string]$r.Detail)") }
        $kept = @(Get-StartRecordLogLines -Record $r)
        foreach ($l in $kept) { [void]$lines.Add("$SubKey log: $l") }
        if (-not $StartRecord -and $null -ne $r.LogLines -and [int64]$r.LogLines -ne $kept.Count) {
            [void]$lines.Add("$SubKey log: the record says $($r.LogLines) line(s) were kept and the key holds $($kept.Count)")
        }
        return @($lines)
    } finally { $k.Dispose() }
}

# The GPU device's state as the installer's verify step read it, as lines. $State is an ordered map; a field that
# could not be read is $null and says so.
function Get-DiagnosticsDeviceLines {
    param([AllowNull()]$State)
    if ($null -eq $State) { return @('device: nothing was read') }
    $lines = New-Object Collections.ArrayList
    [void]$lines.Add("device present: $(if ($null -eq $State.present) { 'unknown' } elseif ([bool]$State.present) { 'yes' } else { 'no' })")
    [void]$lines.Add("device instance: $(if ($State.instance_id) { [string]$State.instance_id } else { 'none' })")
    [void]$lines.Add("device problem code: $(Format-DiagnosticsProblemCode $State.problem_code)")
    [void]$lines.Add("device status: $(if ($State.status) { [string]$State.status } else { 'unreadable' })")
    [void]$lines.Add("device service: $(if ($State.service) { [string]$State.service } else { 'none' })")
    [void]$lines.Add("driver version: $(if ($State.driver_version) { [string]$State.driver_version } else { 'unreadable' }) (the package has $(if ($State.package_version) { [string]$State.package_version } else { 'no version' }))")
    [void]$lines.Add("driver INF: $(if ($State.inf) { [string]$State.inf } else { 'unreadable' })")
    [void]$lines.Add("driver provider: $(if ($State.provider) { [string]$State.provider } else { 'unreadable' })")
    [void]$lines.Add("driver reply version: $(if ($State.reply_version) { [string]$State.reply_version } else { 'none' })")
    [void]$lines.Add("LastStage: $(if ($null -ne $State.last_stage) { [string]$State.last_stage } else { 'none' })")
    [void]$lines.Add("start-health flags: $(if ($null -ne $State.health_flags) { [string]$State.health_flags } else { 'no reading' })")
    return @($lines)
}

# One log of this boot from one provider, newest first, bounded. $Log is the log the provider writes to and $Levels
# are the event levels to take. Returns lines; a provider with no entry of those levels says so, and so does one this
# computer does not know (a driver that writes no event-log entry has no provider here, which is not a failure).
function Get-DiagnosticsEventLines {
    param([Parameter(Mandatory)][string]$Provider, [string]$Log = 'System', [int[]]$Levels = @(1, 2, 3), [AllowNull()]$BootUtc,
          [int]$Max = $script:DiagnosticsMaxEventsPerProvider)
    $label = "$Provider in $Log"
    $filter = @{ LogName = $Log; ProviderName = $Provider; Level = $Levels }
    if ($null -ne $BootUtc) { $filter['StartTime'] = ([datetime]$BootUtc).ToLocalTime() }
    $events = $null
    try { $events = @(Get-WinEvent -FilterHashtable $filter -MaxEvents $Max -ErrorAction Stop) }
    catch {
        $why = [string]$_.Exception.Message
        if ($why -match 'No events were found') { return @("$label : no entry of levels $($Levels -join ',') in this boot") }
        if ($why -match 'not an event provider') { return @("$label : the provider is not registered on this computer") }
        return @("$label : not read ($why)")
    }
    if (-not $events.Count) { return @("$label : no entry of levels $($Levels -join ',') in this boot") }
    $lines = New-Object Collections.ArrayList
    [void]$lines.Add("$label : $($events.Count) entr$(if ($events.Count -eq 1) { 'y' } else { 'ies' }) of levels $($Levels -join ','), newest first (at most $Max)")
    foreach ($e in $events) {
        $message = [string]$e.Message
        if ($message) { $message = ($message -replace '\s+', ' ').Trim() }
        if ($message.Length -gt 400) { $message = $message.Substring(0, 400) + ' ...' }
        [void]$lines.Add(('  {0} level {1} id {2} {3}' -f $e.TimeCreated.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ'), [int]$e.Level, [int]$e.Id, $message))
    }
    return @($lines)
}

# The sections of setupapi.dev.log whose header line names $HardwareId, the newest $MaxSections of them, each cut to
# $MaxLines lines. The file is read line by line: on a tester's PC it is tens of megabytes, and only the kept
# sections are ever held. A section starts at a '>>>  [...]' header and ends at the next one or at its
# '<<<  Section end' line.
function Select-DiagnosticsSetupApiSections {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$HardwareId,
          [int]$MaxSections = $script:DiagnosticsMaxSetupApiSections, [int]$MaxLines = $script:DiagnosticsMaxSetupApiLines,
          [double]$BudgetSeconds = 0)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return [ordered]@{ sections = @(); seen = 0; note = "$Path is not there" } }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $kept = New-Object Collections.ArrayList     # the newest sections, in file order
    $seen = 0
    $current = $null
    $wanted = $false
    $note = $null
    $reader = New-Object IO.StreamReader($Path)
    try {
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { break }
            if ($BudgetSeconds -gt 0 -and $clock.Elapsed.TotalSeconds -ge $BudgetSeconds) {
                $note = "the budget of $BudgetSeconds s ran out inside $Path; the sections above it are the ones that were read"
                break
            }
            if ($line.StartsWith('>>>  [')) {
                if ($wanted -and $null -ne $current) { [void]$kept.Add($current) }
                $wanted = $line.IndexOf($HardwareId, [StringComparison]::OrdinalIgnoreCase) -ge 0
                $current = $null
                if ($wanted) { $seen++; $current = New-Object Collections.ArrayList; [void]$current.Add($line) }
                while ($kept.Count -gt $MaxSections) { $kept.RemoveAt(0) }
                continue
            }
            if (-not $wanted) { continue }
            if ($current.Count -lt $MaxLines) { [void]$current.Add($line) }
            elseif ($current.Count -eq $MaxLines) { [void]$current.Add("  [the section is longer than $MaxLines lines: the rest was left out]") }
            if ($line.StartsWith('<<<  Section end')) {
                [void]$kept.Add($current); $current = $null; $wanted = $false
                while ($kept.Count -gt $MaxSections) { $kept.RemoveAt(0) }
            }
        }
        if ($wanted -and $null -ne $current) { [void]$kept.Add($current) }
        while ($kept.Count -gt $MaxSections) { $kept.RemoveAt(0) }
    } finally { $reader.Dispose() }
    return [ordered]@{ sections = @($kept); seen = $seen; note = $note }
}

# The files of the KMD's log folder, newest first, bounded in number and in lines per file.
function Get-DiagnosticsKmdLogLines {
    param([Parameter(Mandatory)][string]$Directory, [int]$MaxFiles = $script:DiagnosticsMaxKmdLogFiles,
          [int]$MaxLines = $script:DiagnosticsMaxKmdLogLines)
    if (-not (Test-Path -LiteralPath $Directory)) { return @("$Directory is not there (the driver's own log folder of a lab installation)") }
    $files = @(Get-ChildItem -LiteralPath $Directory -File -ErrorAction Stop | Sort-Object LastWriteTimeUtc -Descending)
    if (-not $files.Count) { return @("$Directory is empty") }
    $lines = New-Object Collections.ArrayList
    [void]$lines.Add("$Directory : $($files.Count) file(s), the newest $([Math]::Min($MaxFiles, $files.Count)) below, at most $MaxLines lines each")
    foreach ($f in @($files | Select-Object -First $MaxFiles)) {
        [void]$lines.Add("--- $($f.Name), $($f.Length) bytes, written $($f.LastWriteTimeUtc.ToString('yyyy-MM-ddTHH:mm:ssZ')) ---")
        try {
            $tail = @(Get-Content -LiteralPath $f.FullName -Tail $MaxLines -ErrorAction Stop)
            foreach ($l in $tail) { [void]$lines.Add('  ' + [string]$l) }
        } catch { [void]$lines.Add("  the file could not be read: $($_.Exception.Message)") }
    }
    return @($lines)
}

# ---- the file ---------------------------------------------------------------------------------------------------
# Writes the diagnostics file and returns what happened: path, written, bytes, seconds, cut, sections (each with its
# own outcome) and problems. It never throws: a section that fails writes its reason into the file, and a file that
# cannot be written comes back with written = $false and the reason in problems.
function Write-StartFailureDiagnostics {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)]$Decision,
        [AllowNull()]$DeviceState,
        [AllowNull()]$BootUtc,
        [AllowNull()]$BootId,
        # Where $BootId was read: 'KUSER_SHARED_DATA' or 'PrefetchParameters registry' (Get-StartRecordBootIdSource).
        [AllowNull()][string]$BootIdSource,
        $Root = [Microsoft.Win32.Registry]::LocalMachine,
        [string]$ParametersSubKey = 'SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters',
        [string]$HardwareId = 'PCI\VEN_1002&DEV_13FE',
        [string]$SetupApiLog = (Join-Path $env:windir 'INF\setupapi.dev.log'),
        [string]$KmdLogDir = 'C:\BC250\kmdlog',
        [AllowNull()]$EventSources = $null,
        [int]$BudgetSeconds = $script:DiagnosticsBudgetSeconds,
        [int]$MaxBytes = $script:DiagnosticsMaxBytes,
        [string]$Context = ''
    )
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $sink = New-DiagnosticsSink -MaxBytes $MaxBytes
    $problems = New-Object Collections.ArrayList
    $sections = New-Object Collections.ArrayList
    if ($null -eq $EventSources) { $EventSources = $script:DiagnosticsEventSources }
    function Add-Section {
        param([string]$Name, [scriptblock]$Body)
        $left = $BudgetSeconds - $clock.Elapsed.TotalSeconds
        [void](Add-DiagnosticsLine -Sink $sink -Text '' -Where $Name)
        [void](Add-DiagnosticsLine -Sink $sink -Text "== $Name ==" -Where $Name)
        if ($sink.cut) { [void]$sections.Add("$Name : not collected, the size bound was reached"); return }
        if ($left -le 0.5) {
            [void](Add-DiagnosticsLine -Sink $sink -Text "[the $BudgetSeconds s budget ran out before this section: nothing of it was collected]" -Where $Name)
            [void]$sections.Add("$Name : not collected, the time budget ran out")
            [void]$problems.Add("$Name : the time budget ran out")
            return
        }
        try {
            $lines = @(& $Body ([double]$left))
            [void](Add-DiagnosticsLines -Sink $sink -Lines $lines -Where $Name)
            [void]$sections.Add("$Name : $(@($lines).Count) line(s)")
        } catch {
            $why = [string]$_.Exception.Message
            [void](Add-DiagnosticsLine -Sink $sink -Text "[this section could not be collected: $why]" -Where $Name)
            [void]$sections.Add("$Name : failed, $why")
            [void]$problems.Add("$Name : $why")
        }
    }

    [void](Add-DiagnosticsLine -Sink $sink -Text "schema $($script:DiagnosticsSchema)")
    [void](Add-DiagnosticsLine -Sink $sink -Text "written $([DateTime]::UtcNow.ToString('o')) by the amdgpu-wddm installer, because the driver did not start")
    [void](Add-DiagnosticsLine -Sink $sink -Text "this boot started $(if ($null -ne $BootUtc) { ([datetime]$BootUtc).ToString('o') } else { 'at a time that cannot be read' }), BootId $(if ($null -ne $BootId) { [string]$BootId } else { 'cannot be read' })$(if ($BootIdSource) { " (read from $BootIdSource)" } else { '' })")
    if ($Context) { [void](Add-DiagnosticsLine -Sink $sink -Text "context $Context") }
    [void](Add-DiagnosticsLine -Sink $sink -Text "bounds $BudgetSeconds s and $([int]($MaxBytes / 1024)) KiB for the whole file")
    [void](Add-DiagnosticsLine -Sink $sink -Text '')
    [void](Add-DiagnosticsLine -Sink $sink -Text '== the decision ==')
    [void](Add-DiagnosticsLine -Sink $sink -Text $([string]$Decision.summary))
    foreach ($r in @($Decision.reasons)) { [void](Add-DiagnosticsLine -Sink $sink -Text "reason: $r") }
    foreach ($r in @($Decision.notes)) { [void](Add-DiagnosticsLine -Sink $sink -Text "note: $r") }

    Add-Section 'the driver device' { Get-DiagnosticsDeviceLines -State $DeviceState }
    Add-Section 'the start record' { Get-DiagnosticsRecordLines -Root $Root -ParametersSubKey $ParametersSubKey -SubKey $script:StartRecordKey -BootUtc $BootUtc -StartRecord }
    Add-Section 'the stop record' { Get-DiagnosticsRecordLines -Root $Root -ParametersSubKey $ParametersSubKey -SubKey $script:StopRecordKey -BootUtc $BootUtc -BootId $BootId -BootIdSource $BootIdSource }
    Add-Section 'the event logs of this boot' {
        param([double]$Left)
        $out = New-Object Collections.ArrayList
        foreach ($s in @($EventSources)) {
            if ($clock.Elapsed.TotalSeconds -ge $BudgetSeconds) { [void]$out.Add("[the $BudgetSeconds s budget ran out: the sources after $($s.provider) were not read]"); break }
            $log = $(if ($s.log) { [string]$s.log } else { 'System' })
            foreach ($l in (Get-DiagnosticsEventLines -Provider ([string]$s.provider) -Log $log -Levels @($s.levels) -BootUtc $BootUtc)) { [void]$out.Add($l) }
        }
        return @($out)
    }
    Add-Section 'setupapi.dev.log, the sections of this device' {
        param([double]$Left)
        $r = Select-DiagnosticsSetupApiSections -Path $SetupApiLog -HardwareId $HardwareId -BudgetSeconds ([Math]::Min($Left, 10))
        $out = New-Object Collections.ArrayList
        [void]$out.Add("$SetupApiLog : $($r.seen) section(s) name $HardwareId; the newest $(@($r.sections).Count) below")
        if ($r.note) { [void]$out.Add("[$($r.note)]") }
        foreach ($s in @($r.sections)) { foreach ($l in @($s)) { [void]$out.Add([string]$l) } }
        return @($out)
    }
    Add-Section "the driver's own log folder" { Get-DiagnosticsKmdLogLines -Directory $KmdLogDir }

    [void](Add-DiagnosticsLine -Sink $sink -Text '')
    [void](Add-DiagnosticsLine -Sink $sink -Text "== the collection ==")
    foreach ($s in @($sections)) { [void](Add-DiagnosticsLine -Sink $sink -Text "section $s") }
    # The invariant culture, so that one file reads the same on every tester's PC and a gate can match it.
    [void](Add-DiagnosticsLine -Sink $sink -Text ([string]::Format([cultureinfo]::InvariantCulture,
        'took {0:F1} s of the {1} s budget; {2} of {3} KiB', $clock.Elapsed.TotalSeconds, $BudgetSeconds, [int]($sink.bytes / 1024), [int]($MaxBytes / 1024))))
    $written = $false
    $tmp = $null
    try {
        [void][IO.Directory]::CreateDirectory((Split-Path $Path))
        $tmp = $Path + '.tmp-' + [guid]::NewGuid().ToString('N')
        [IO.File]::WriteAllText($tmp, $sink.text.ToString(), (New-Object Text.UTF8Encoding $false))
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) } else { [IO.File]::Move($tmp, $Path) }
        $written = $true
    } catch { [void]$problems.Add("the file could not be written: $($_.Exception.Message)") }
    finally {
        # The staging file goes whatever happened to the move or the replace. It is written into the installer's
        # state folder, and three gates of this tree compare the files of that folder before and after themselves:
        # one orphan .tmp-<guid> of a failed write would fail the next gate, not this collection.
        if (-not $written -and $tmp) { try { if ([IO.File]::Exists($tmp)) { [IO.File]::Delete($tmp) } } catch { } }
    }
    return [ordered]@{
        path = $(if ($written) { $Path } else { $null })
        written = $written
        bytes = $sink.bytes
        seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 1)
        cut = [bool]$sink.cut
        sections = @($sections)
        problems = @($problems)
    }
}
