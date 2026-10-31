# BD-122: the consumer side of the KMD's start record (Parameters\StartRecord). The driver's own half is the host
# test driver\kmd\test\run_start_record.ps1; this one holds the readers and the words they print:
#
#   powershell -NoProfile -File tools\release\test-start-record.ps1 [-Installer <package>\installer] [-Out <dir>]
#
# The fixtures are written in an in-memory typed record store and read back
# through the one reader that start-confirm.ps1 and the installer use:
#   a step failure (the GitHub #38 shape: Code 43, LastStage 91, now with the step and the NTSTATUS),
#   the boot-loop guard's refusal,
#   a start that succeeded after a failure (the stale reason must not read as the current one),
#   a record an earlier boot left, and one written now,
#   the record of the field report 20261011T222253Z, which this boot's own start time must not disown,
#   a stop record with the seven values of its own, from this boot and from an earlier one, and a torn one,
#   no record at all (a KMD before 0.7.216.30).
# Then the extraction the installer's verify step performs on the probe's output, and the source rules that keep
# both consumers honest. Runs under Windows PowerShell 5.1 and PowerShell 7 without host registry access.
param([string]$Installer = (Join-Path (Split-Path $PSScriptRoot) 'release\installer'), [string]$Out = '')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $Installer 'start-confirm-core.ps1'))) {
    $Installer = Join-Path $PSScriptRoot 'installer'
}
. (Join-Path $Installer 'start-confirm-core.ps1')
. (Join-Path $Installer 'state-reader.ps1')
. (Join-Path $PSScriptRoot 'test-record-store.ps1')
$script:FixtureBootUtc=[datetime]::UtcNow.AddHours(-1)
function Get-BootIdentity {return @{boot_id=387;boot_utc=$script:FixtureBootUtc.ToString('o')}}
function Get-CimInstance {return [pscustomobject]@{LastBootUpTime=$script:FixtureBootUtc}}
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

# The scratch key has a suffix of its own, so two dry runs at the same time (the two flavours of a package run)
# never write, read back or remove each other's key.
$scratch = 'Software\amdgpu-wddm-start-record-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$root = New-RecordFixtureKey
$root.DeleteSubKeyTree($scratch, $false)
function New-Fixture([string]$Name, [hashtable]$Values) {
    $k = $root.CreateSubKey("$scratch\$Name")
    try {
        foreach ($n in $Values.Keys) {
            $v = $Values[$n]
            if ($v -is [string[]]) { $k.SetValue($n, $v, [Microsoft.Win32.RegistryValueKind]::MultiString) }
            elseif ($v -is [int]) { $k.SetValue($n, $v, [Microsoft.Win32.RegistryValueKind]::DWord) }
            else { $k.SetValue($n, [string]$v, [Microsoft.Win32.RegistryValueKind]::String) }
        }
    } finally { $k.Dispose() }
}
function Read-Fixture([string]$Name) {
    $k = $root.OpenSubKey("$scratch\$Name")
    if (-not $k) { return $null }
    try { return Read-StartRecordFromKey $k } finally { $k.Dispose() }
}

try {
    # The widest line the ring can hold, and one over it: the reader trims, it never refuses a record.
    $wide = 'x' * 159
    $tooWide = 'y' * 200
    New-Fixture 'failure' @{
        Result = 'failed'; Step = 'DisplayMapFramebuffer'; SubStep = '-'; TimeUtc = '2026-10-11T11:52:40.123Z'
        Status = [int]-1073741790                  # 0xC0000022 STATUS_ACCESS_DENIED, as a REG_DWORD reads back
        Stage = [int]91; UnconfirmedStarts = [int]1; Version = [int]0x000700D8
        Log = [string[]]@('post display 1920x1080 pitch 7680 format 1 target 1', $wide, $tooWide, 'start failed 0xC0000022')
    }
    New-Fixture 'guard' @{
        Result = 'guard'; Step = 'BootLoopGuard'; SubStep = '-'; TimeUtc = '2026-10-11T11:43:30.001Z'
        Status = [int]-1073741438                  # 0xC0000182 STATUS_DEVICE_CONFIGURATION_ERROR
        Stage = [int]90; UnconfirmedStarts = [int]2; Version = [int]0x000700D8
        Log = [string[]]@('guard: 2 starts nobody confirmed, refusing to start')
    }
    New-Fixture 'started' @{
        Result = 'started'; Step = '-'; SubStep = '-'; TimeUtc = '2026-10-11T12:05:00.000Z'
        Status = [int]0; Stage = [int]39; UnconfirmedStarts = [int]1; Version = [int]0x000700D8
    }
    New-Fixture 'timing' @{
        Result = 'failed'; Step = 'DisplayPrepareInheritedTiming'; SubStep = '-'; TimeUtc = '2026-10-11T14:18:58.703Z'
        Status = [int]-1073741637                  # 0xC00000BB STATUS_NOT_SUPPORTED
        Stage = [int]91; UnconfirmedStarts = [int]0; Version = [int]0x000700D8
        Detail = 'timing 0xC00000BB post 800x600 pitch 3328 otg0 ctl 00000000 otg1 ctl 00000001'
        Log = [string[]]@('start failed 0xC00000BB')
    }
    # The driver writes an attempt record at the top of a start, before any step runs, so it names no step: its
    # Step is "-" and its Stage is 30 (StageStartEnter). Nothing else can stand there.
    New-Fixture 'inside' @{
        Result = 'attempt'; Step = '-'; SubStep = '-'; TimeUtc = '2026-10-11T12:09:00.000Z'
        Status = [int]0; Stage = [int]30; UnconfirmedStarts = [int]2; Version = [int]0x000700D8
    }
    # A failure inside WddmStart: the sub-step of GpuStartupInitialize travels with the step.
    New-Fixture 'substep' @{
        Result = 'failed'; Step = 'WddmStart'; SubStep = 'psp-initialize'; TimeUtc = '2026-10-11T12:09:00.000Z'
        Status = [int]-1073741304                  # 0xC00000A3 STATUS_DEVICE_NOT_READY
        Stage = [int]91; UnconfirmedStarts = [int]1; Version = [int]0x000700D8
        Log = [string[]]@('startup: entering firmware preparation', 'start failed 0xC00000A3')
    }
    # The same machine one boot later, with a record the previous boot left: a start refused above the attempt
    # record (a device object the driver does not own) writes nothing, so this reason outlives its own boot.
    New-Fixture 'lastboot' @{
        Result = 'failed'; Step = 'SmuOwnerStart'; SubStep = '-'; TimeUtc = '2020-01-02T03:04:05.000Z'
        Status = [int]-1073741790; Stage = [int]91; UnconfirmedStarts = [int]0; Version = [int]0x000700D8
        Log = [string[]]@('smu: native owner start status 0xC0000022')
    }
    # The same shape written at this moment: whatever this PC's boot time is, this one belongs to this boot.
    New-Fixture 'now' @{
        Result = 'failed'; Step = 'SmuOwnerStart'; SubStep = '-'
        TimeUtc = [datetime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        Status = [int]-1073741790; Stage = [int]91; UnconfirmedStarts = [int]0; Version = [int]0x000700D8
        Log = [string[]]@('smu: native owner start status 0xC0000022')
    }

    '1. a step failure: the step, the status by name, the stage, the count and the kept lines'
    $r = Read-Fixture 'failure'
    Check ($null -ne $r) 'the record is read back'
    $t = Format-StartRecord $r
    Check ($t -match 'FAILED in DisplayMapFramebuffer') 'the one line names the failing step'
    Check ($t -match '0xC0000022 \(STATUS_ACCESS_DENIED\)') 'and the NTSTATUS, by number and by name'
    Check ($t -match 'could not map the firmware framebuffer') 'and says what that step is, in plain words'
    Check ($t -match 'stage 91,') 'and the stage a tester already sees in start-confirm.log'
    Check ($t -match 'UnconfirmedStarts 1') 'and the boot-loop count, which tells a failure from a guard refusal'
    Check ($t -match '4 log lines kept') 'and how many driver log lines came with it'
    Check (Test-StartRecordIsReason $r) 'a failure is a reason for the start that ran'
    $lines = @(Get-StartRecordLogLines -Record $r)
    Check ($lines.Count -eq 4) 'every kept line is handed back'
    Check ($lines[0] -eq 'post display 1920x1080 pitch 7680 format 1 target 1') 'oldest first, as the ring had them'
    Check ($lines[1].Length -eq 159 -and $lines[2].Length -eq 159) 'a line is at most 159 characters, the ring line'
    Check (@(Get-StartRecordLogLines -Record $r -Max 2).Count -eq 2) 'a caller can ask for the last few only'
    Check ((Get-StartRecordLogLines -Record $r -Max 2)[-1] -eq 'start failed 0xC0000022') 'and gets the newest ones'

    '1b. GitHub #38: the measured values of the refusing step travel with the failure'
    $t = Format-StartRecord (Read-Fixture 'timing')
    Check ($t -match 'FAILED in DisplayPrepareInheritedTiming') 'the timing step is named'
    Check ($t -match 'detail: timing 0xC00000BB post 800x600 pitch 3328 otg0 ctl 00000000 otg1 ctl 00000001$') 'and its registers end the line'
    Check (-not ((Format-StartRecord (Read-Fixture 'failure')) -match 'detail:')) 'a failure without Detail prints no empty detail'

    '2. a sub-step inside WddmStart, and the attempt record that names no step at all'
    $t = Format-StartRecord (Read-Fixture 'substep')
    Check ($t -match 'FAILED in WddmStart / psp-initialize') 'the step and the sub-step'
    Check ($t -match 'the GPU did not finish starting') 'and the step in plain words'
    $t = Format-StartRecord (Read-Fixture 'inside')
    Check ($t -match 'neither failed nor finished') 'an attempt record says the machine stopped inside the start'
    Check ($t -match 'names no step') 'and says it names no step, instead of printing one it does not have'
    Check ($t -match 'LastStage and StageHistory') 'and points at the two values that do say how far the start got'
    Check ($t -match 'UnconfirmedStarts 2') 'it still carries the count that decides the next start'
    Check ($t -match 'driver ABI 0x000700D8') 'and the escape ABI word of the driver that wrote it'
    Check (-not (Test-StartRecordIsReason (Read-Fixture 'inside'))) 'and it is not read as a failure reason'

    '3. the boot-loop guard'
    $r = Read-Fixture 'guard'
    $t = Format-StartRecord $r
    Check ($t -match 'REFUSED by the driver.s own boot-loop guard') 'the refusal says it was the guard'
    Check ($t -match 'UnconfirmedStarts 2') 'and how many starts nobody confirmed'
    Check ($t -match '0xC0000182 \(STATUS_DEVICE_CONFIGURATION_ERROR\)') 'and the status it answered'
    Check (Test-StartRecordIsReason $r) 'a refusal is a reason too'

    '4. a start that succeeded, after a failure: the stale reason can never read as the current one'
    $r = Read-Fixture 'started'
    $t = Format-StartRecord $r
    Check ($t -match 'reached a started device') 'the success is stated as a success'
    Check ($t -notmatch 'FAILED' -and $t -notmatch 'REFUSED') 'and names no step and no status'
    Check ($t -match 'UnconfirmedStarts 1' -and $t -match 'driver ABI 0x000700D8') 'the count and the ABI word are there too'
    Check (-not (Test-StartRecordIsReason $r)) 'and is not a reason'
    Check (@(Get-StartRecordLogLines -Record $r).Count -eq 0) 'the failure lines are gone with it'

    '4b. a record the previous boot left behind (the one case the driver''s rule cannot cover)'
    $boot = Get-StartRecordBootUtc
    Check ($null -ne $boot -and $boot -lt [datetime]::UtcNow) 'this boot''s start time is read'
    $r = Read-Fixture 'lastboot'
    $t = Format-StartRecord -Record $r -BootUtc $boot
    Check ($t -match 'FAILED in SmuOwnerStart') 'the reason is still printed'
    Check ($t -match 'not about any start of this boot') 'and is marked as older than this boot'
    Check (-not (Test-StartRecordIsReason -Record $r -BootUtc $boot)) 'so it is not this start''s reason'
    Check (Test-StartRecordIsReason -Record $r) 'without a boot time nothing is claimed either way'
    $now = Read-Fixture 'now'
    Check ((Format-StartRecord -Record $now -BootUtc $boot) -notmatch 'not about any start') 'a record written now is not marked'
    Check (Test-StartRecordIsReason -Record $now -BootUtc $boot) 'and is read as this start''s reason'
    Check ((Format-StartRecord -Record $now -BootUtc ([datetime]::UtcNow.AddDays(1))) -match 'not about any start') 'a record older than the boot always is'

    '4c. where this boot''s start time comes from: Get-BootIdentity, never the present moment'
    # The field defect: in .NET Framework, the runtime of Windows PowerShell 5.1 and so of every installer and task
    # run on a tester's PC, [Environment]::TickCount64 does not exist. The old reading subtracted $null from
    # [datetime]::UtcNow, got the present moment, and marked every record of this boot as a foreign one.
    Check ($PSVersionTable.PSVersion.Major -ne 5 -or -not [bool][string]([Environment]::TickCount64)) 'this shell has no TickCount64, which is why the tick count reading failed in the field'
    $os = $null
    try { $os = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime() } catch { }
    Check ($null -eq $os -or [Math]::Abs(($boot - $os).TotalSeconds) -lt 2) 'the time read is Win32_OperatingSystem LastBootUpTime, the one definition of this boot'
    Check (([datetime]::UtcNow - $boot).TotalSeconds -gt 2 -or $null -eq $os) 'and not the present moment'
    # The two answers Get-BootIdentity can give, injected in a child scope: the function is resolved at the call, so
    # this holds the real reader and needs no boot of its own.
    $given = & { function Get-BootIdentity { return [ordered]@{ boot_id = 11; boot_utc = '2026-10-11T22:19:45.5000000Z' } }; Get-StartRecordBootUtc }
    Check ($given -eq ([datetime]::Parse('2026-10-11T22:19:45.5000000Z', [cultureinfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor [System.Globalization.DateTimeStyles]::AssumeUniversal))) 'a boot identity with boot_utc gives exactly that time, in UTC'
    $missing = & { function Get-BootIdentity { return [ordered]@{ boot_id = 11; boot_utc = $null } }; Get-StartRecordBootUtc }
    Check ($null -eq $missing) 'a boot identity whose boot_utc cannot be read gives nothing, never the present moment'
    # The fallback, in a process that loads start-confirm-core.ps1 alone: it dot-sources release-witness.ps1 inside
    # the function, so Get-BootIdentity is gone again on return and that script's own "read once per process" cache
    # is reset with it. The reading is therefore kept here, and a second call answers from it instead of paying
    # another Win32_OperatingSystem query (seconds on a tester's PC). A child process is the only honest witness:
    # this one has the function loaded already.
    $probeFile = Join-Path ([IO.Path]::GetTempPath()) ('amdgpu-bootutc-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.ps1')
    $probeBody = @(
        '$ErrorActionPreference = ''Stop''',
        'function Get-ItemProperty { return [pscustomobject]@{BootId=387} }',
        'function Get-CimInstance { return [pscustomobject]@{LastBootUpTime=[datetime]''2026-10-11T22:19:45Z''} }',
        ". '$(Join-Path $Installer 'start-confirm-core.ps1')'",
        '$before = [bool](Get-Command -Name Get-BootIdentity -CommandType Function -ErrorAction SilentlyContinue)',
        '$c1 = [Diagnostics.Stopwatch]::StartNew(); $a = Get-StartRecordBootUtc; $t1 = [int]$c1.ElapsedMilliseconds',
        '$c2 = [Diagnostics.Stopwatch]::StartNew(); $b = Get-StartRecordBootUtc; $t2 = [int]$c2.ElapsedMilliseconds',
        # Whole milliseconds, so that the reading travels through no decimal separator of a locale.
        '"{0}|{1}|{2}|{3}|{4}" -f $before, $(if ($null -ne $a) { $a.ToString(''o'') } else { ''none'' }), $(if ($null -ne $b) { $b.ToString(''o'') } else { ''none'' }), $t1, $t2'
    )
    [IO.File]::WriteAllLines($probeFile, [string[]]$probeBody)
    try {
        $probeOut = @(& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $probeFile | ForEach-Object { [string]$_ })
        $parts = @(($probeOut | Where-Object { $_ -match '\|' } | Select-Object -Last 1) -split '\|')
        Check (@($parts).Count -eq 5 -and $parts[0] -eq 'False') "the child process loads the core alone, without Get-BootIdentity ($($probeOut -join ' '))"
        Check (@($parts).Count -eq 5 -and $parts[1] -eq $parts[2] -and $parts[1] -ne 'none') 'the fallback reads this boot''s start time and gives the same answer twice'
        Check (@($parts).Count -eq 5 -and [int]$parts[4] -le 250) "and the second call answers from the kept reading, not from another CIM query ($($parts[3]) ms then $($parts[4]) ms)"
    } finally { Remove-Item -LiteralPath $probeFile -Force -ErrorAction SilentlyContinue }

    '4d. the field case 20261011T222253Z: the record of this boot was marked as a foreign one'
    # The values of run-1-result.json of that report: boot_utc 22:19:45.5Z, the record written 6.8 s later, while the
    # device stood at problem code 43 with LastStage 79. Before the fix the boot time was "now", so the note below
    # was printed and the record read as another boot's.
    $fieldBoot = & { function Get-BootIdentity { return [ordered]@{ boot_id = 11; boot_utc = '2026-10-11T22:19:45.5000000Z' } }; Get-StartRecordBootUtc }
    New-Fixture 'field' @{
        Result = 'started'; Step = '-'; SubStep = '-'; TimeUtc = '2026-10-11T22:19:52.317Z'
        Status = [int]0; Stage = [int]39; UnconfirmedStarts = [int]1; Version = [int]0x000700D8
    }
    $f = Read-Fixture 'field'
    Check (-not (Test-StartRecordFromEarlierBoot -Record $f -BootUtc $fieldBoot)) 'a record written 6.8 s after the boot is a record of this boot'
    Check ((Format-StartRecord -Record $f -BootUtc $fieldBoot) -notmatch 'not about any start') 'and its line carries no note about another boot'
    Check ((Format-StartRecord -Record $f -BootUtc ([datetime]::Parse('2026-10-11T22:19:52.317Z', [cultureinfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor [System.Globalization.DateTimeStyles]::AssumeUniversal))) -notmatch 'not about any start') 'a boot time of the record''s own moment does not mark it either (the two-minute margin)'

    '4e. the stop record (KMD 0.7.216.39): its own reader, its own words, and an absent one is normal'
    # The record the KMD writes when Windows stops a device whose start nothing had confirmed (guard.c
    # GuardRecordStopBegin and GuardRecordStopDone, HANDOFF38 "Record contract"). The seven values of its own are
    # here with the registry types the driver writes: StartStage, HealthFlags, BootId and LogLines as REG_DWORD,
    # and Generation, Epoch and StopTimeUtc as REG_SZ. The generation and the epoch are 64-bit numbers, so a
    # reader that parses them into a DWORD loses digits: these fixtures carry values no DWORD can hold.
    New-Fixture 'stoprecord' @{
        Result = 'stopped'; Step = 'StopDevice'; SubStep = 'unconfirmed start'; TimeUtc = '2026-10-11T22:19:53.100Z'
        Detail = 'Windows stopped the device; StopDevice supplies no reason code.'
        Status = [int]0; Stage = [int]79; StartStage = [int]39; HealthFlags = [int]3; BootId = [int]11
        Generation = '18446744073709551610'; Epoch = '4294967297'; StopTimeUtc = '2026-10-11T22:19:53.480Z'
        LogLines = [int]2; Version = [int]0x000700D8
        Log = [string[]]@('stop: dxgkrnl asked for a stop 1.1 s after the start', 'stop: done')
    }
    $stopKey = $root.OpenSubKey("$scratch\stoprecord")
    try { $stop = Read-StopRecordFromKey $stopKey } finally { $stopKey.Dispose() }
    Check ($null -ne $stop -and [string]$stop.Result -eq 'stopped' -and [int]$stop.Stage -eq 79) 'the stop reader reads the shared values'
    Check ([int]$stop.StartStage -eq 39 -and [int]$stop.HealthFlags -eq 3 -and [int]$stop.BootId -eq 11 -and [int]$stop.LogLines -eq 2) 'and the four DWORDs of its own: StartStage, HealthFlags, BootId, LogLines'
    Check ([string]$stop.Generation -eq '18446744073709551610' -and [string]$stop.Epoch -eq '4294967297') 'the generation and the epoch keep all their digits, as the decimal strings the driver writes'
    Check ([string]$stop.StopTimeUtc -eq '2026-10-11T22:19:53.480Z') 'and the teardown''s own finish time is read'
    Check (@(Get-StartRecordLogLines -Record $stop).Count -eq 2) 'the kept ring lines come back'
    Check (-not (Test-StartRecordIsReason -Record $stop -BootUtc $fieldBoot)) 'a stop record is never read as a start reason'
    $stopLine = Format-StopRecord -Record $stop -BootUtc $fieldBoot -BootId 11
    Check ($stopLine -match 'stage 39 before the stop' -and $stopLine -match 'stop stage 79') 'the line names the stage before the stop and the stage the stop reached'
    Check ($stopLine -match 'start-health flags 3, so the start was not full, ready and visible') 'and says what the health flags mean'
    Check ($stopLine -match 'generation 18446744073709551610' -and $stopLine -match 'epoch 4294967297') 'the generation and the epoch are in the line, undamaged'
    Check ($stopLine -match 'which is no reason: StopDevice gives the driver no code') 'Status 0 is printed with its meaning beside it and never as a success'
    Check ($stopLine -notmatch 'STATUS_SUCCESS[^,]*$') 'so no reader of the line can end on the word success'
    Check ($stopLine -match 'driver ABI 0x000700D8') 'the version word is named as the escape ABI word, which two releases of one ABI share'

    '4f. this boot or an earlier one: the question a stop record cannot answer about itself'
    # A stop of a start that WAS confirmed writes nothing and leaves an older record standing, so the reader must
    # place every stop record in a boot before it shows it. The BootId decides when both sides have one, because a
    # clock that moved cannot change it; the time decides when it does not.
    Check ((Test-StopRecordBoot -Record $stop -BootUtc $fieldBoot -BootId 11) -eq 'this boot') 'the same BootId is this boot'
    Check ((Format-StopRecord -Record $stop -BootUtc $fieldBoot -BootId 11) -notmatch 'EARLIER boot') 'and the line of such a record carries no note'
    Check ((Test-StopRecordBoot -Record $stop -BootUtc $fieldBoot -BootId 12) -eq 'earlier boot') 'another BootId is an earlier boot, whatever the record''s time says'
    Check ((Format-StopRecord -Record $stop -BootUtc $fieldBoot -BootId 12) -match 'NOTE this record is from an EARLIER boot') 'and the line says so'
    New-Fixture 'stopold' @{
        Result = 'stopped'; Step = 'StopDevice'; SubStep = 'unconfirmed start'; TimeUtc = '2026-10-10T08:00:00.000Z'
        Status = [int]0; Stage = [int]79; StartStage = [int]39; HealthFlags = [int]1
        Generation = '7'; Epoch = '1'; LogLines = [int]0; Version = [int]0x000700D8
    }
    $oldKey = $root.OpenSubKey("$scratch\stopold")
    try { $old = Read-StopRecordFromKey $oldKey } finally { $oldKey.Dispose() }
    Check ($null -eq $old.BootId) 'a record of a driver that wrote no BootId reads that value as absent'
    Check ((Test-StopRecordBoot -Record $old -BootUtc $fieldBoot -BootId 11) -eq 'earlier boot') 'and then the time places it: a day before this boot is an earlier boot'
    Check ((Test-StopRecordBoot -Record $old -BootUtc $null -BootId $null) -eq 'unknown') 'with neither a BootId nor a boot time nothing can place it'
    Check ((Format-StopRecord -Record $old -BootUtc $null -BootId $null) -match 'NOTE neither the BootId nor the boot time') 'and the line says that instead of claiming this boot'
    Check ((Format-StopRecord -Record $old -BootUtc $fieldBoot -BootId 11) -match 'the teardown wrote no finish time') 'a record without StopTimeUtc says so'
    New-Fixture 'stopnoresult' @{ Stage = [int]70; StartStage = [int]39; TimeUtc = '2026-10-11T22:19:53.100Z' }
    $k = $root.OpenSubKey("$scratch\stopnoresult")
    try {
        Check ($null -eq (Read-StartRecordFromKey $k)) 'a key without Result is no record for a reader that decides'
        $any = Read-StopRecordFromKey $k
        Check ($null -ne $any -and [int]$any.Stage -eq 70 -and [int]$any.StartStage -eq 39) 'and the stop reader hands back what the key does hold, for the diagnostics file'
        Check ((Format-StopRecord -Record $any -BootUtc $fieldBoot -BootId 11) -match 'the write was torn') 'whose line calls a torn write a torn write'
    } finally { $k.Dispose() }
    Check ($null -eq (Read-Fixture 'stoprecord-absent')) 'an absent stop record reads as nothing at all, which is the normal case'
    Check ((Format-StopRecord -Record $null -BootUtc $fieldBoot -BootId 11) -match 'Windows stopped no unconfirmed start of this boot') 'and the line for an absent record names the normal case first'
    Check ((Format-StopRecord -Record $null -BootUtc $fieldBoot -BootId 11) -match 'the last start failed and is in StartRecord instead') 'and names the other normal case: a start that failed unwinds through stop and writes no stop record'

    '4g. BD-130: where THIS boot''s BootId comes from, and the number that cannot be an earlier boot'
    # The field case: the driver writes KUSER_SHARED_DATA.BootId into the record, this reader took the number from
    # the PrefetchParameters registry copy, and that copy lags inside a boot. On the lab (22631, 13 minutes after
    # the boot) the page said 388 and the registry 387, so the FRESH record of this boot was shown as an earlier
    # boot's. The reading now comes from the page, with the registry only as a fallback, and both readers name the
    # source they used.
    $savedShared = $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID
    try {
        $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID = '388'
        $live = 388
        Check ($live -gt 0) 'the synthetic shared page supplies a positive BootId'
        Check ((Get-SharedBootId) -eq $live) 'and the reader of that page gives exactly that number'
        $reading = Get-StartRecordBootIdReading
        Check ([string]$reading.source -ceq 'KUSER_SHARED_DATA' -and [int64]$reading.boot_id -eq $live) 'so this boot''s reading comes from the page, and says so'
        Check ((Get-StartRecordBootId) -eq $live -and (Get-StartRecordBootIdSource) -ceq 'KUSER_SHARED_DATA') 'the number and the source are the two halves of one reading'
        $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID = '388'
        Check ((Get-SharedBootId) -eq 388 -and (Get-StartRecordBootId) -eq 388) 'the value of the page is the answer whatever the registry says'
        # A host where the page cannot be read falls back to the registry value, through Get-BootIdentity, and the
        # reader says that it did: that answer can be one behind this boot.
        $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID = 'unreadable'
        Check ($null -eq (Get-SharedBootId)) 'a host that cannot read the page gives no number from it'
        $fallback = & { function Get-BootIdentity { return [ordered]@{ boot_id = 387; boot_utc = '2026-10-12T07:23:00.0000000Z' } }; Get-StartRecordBootIdReading }
        Check ([int64]$fallback.boot_id -eq 387 -and [string]$fallback.source -ceq 'PrefetchParameters registry') 'and then the registry value answers, named as the weaker source'
        $none = & { function Get-BootIdentity { return [ordered]@{ boot_id = $null; boot_utc = $null } }; Get-StartRecordBootIdReading }
        Check ($null -eq $none.boot_id -and [string]$none.source -ceq 'nothing') 'a host that answers neither gives no number and names no source'
    } finally { $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID = $savedShared }
    # The record of the field case against both numbers: 388 is this boot, 387 is not an earlier boot at all.
    New-Fixture 'stop388' @{
        Result = 'stopped'; Step = 'StopDevice'; SubStep = 'unconfirmed start'; TimeUtc = '2026-10-12T07:36:24.552Z'
        StopTimeUtc = '2026-10-12T07:36:25.342Z'; Status = [int]0; Stage = [int]79; StartStage = [int]50
        HealthFlags = [int]3; BootId = [int]388; Generation = '1273129079'; Epoch = '4'; LogLines = [int]64
        Version = [int]0x000700D8
    }
    $labKey = $root.OpenSubKey("$scratch\stop388")
    try { $lab = Read-StopRecordFromKey $labKey } finally { $labKey.Dispose() }
    Check ((Test-StopRecordBoot -Record $lab -BootUtc $fieldBoot -BootId 388) -eq 'this boot') 'the record of the field case is this boot''s against the number the page gives'
    Check ((Format-StopRecord -Record $lab -BootUtc $fieldBoot -BootId 388) -notmatch 'NOTE') 'and its line carries no note at all'
    Check ((Test-StopRecordBoot -Record $lab -BootUtc $fieldBoot -BootId 387) -eq 'later boot') 'against the lagging registry number it is no earlier boot: a higher number cannot be one'
    $laterLine = Format-StopRecord -Record $lab -BootUtc $fieldBoot -BootId 387
    Check ($laterLine -match "the record's BootId is HIGHER than the number read for this boot") 'and the line says what the two numbers mean instead of blaming a boot'
    Check ($laterLine -cnotmatch 'from an EARLIER boot') 'never the sentence of the field defect'

    '5. no record at all: a KMD before 0.7.216.30'
    Check ($null -eq (Read-Fixture 'absent')) 'an absent key reads as no record'
    Check ((Format-StartRecord $null) -match 'does not keep one') 'and says so, instead of looking like a success'
    Check (-not (Test-StartRecordIsReason $null)) 'and is not a reason'
    New-Fixture 'empty' @{ Stage = [int]91 }
    Check ($null -eq (Read-Fixture 'empty')) 'a key without Result is no record either'

    '6. what the installer''s verify step extracts from the probe'
    # The three patterns of install.ps1, as that file holds them: each anchored to the start of its own line, so a
    # driver log line in the middle of the probe's output cannot answer for the probe. The last kept line here is a
    # deliberately hostile one - it contains the words of all three readings.
    $probe = @(
        '2026-10-11T11:55:06.951Z +   0.2s probe: DpmMode 1 DpmMaxMHz 2000'
        '2026-10-11T11:55:07.942Z +   1.2s probe: fallback view: device problem 43, driver version  (expected 0x000700D8), LastStage 91'
        '2026-10-11T11:55:08.100Z +   1.4s start record: ' + (Format-StartRecord (Read-Fixture 'failure'))
        '2026-10-11T11:55:08.200Z +   1.5s start record log: start failed 0xC0000022'
        '2026-10-11T11:55:08.210Z +   1.5s start record log: probe: start health flags 0 fallback view: nonsense; start record: nonsense'
        '2026-10-11T11:55:08.539Z +   1.8s probe: start health no reading: start health read refused, status 0xC000000E'
    ) -join "`r`n"
    $view = $null; if ($probe -match '(?m)^(?!.*start record log: ).*probe: fallback view: (.+)$') { $view = $Matches[1].Trim() }
    $record = $null; if ($probe -match '(?m)^(?!.*start record log: ).*\bstart record: (.+)$') { $record = $Matches[1].Trim() }
    $health = $null; if ($probe -match '(?m)^(?!.*start record log: ).*probe: start health (.+)$') { $health = $Matches[1].Trim() }
    Check ($view -match 'LastStage 91') 'the old view still parses, unchanged'
    Check ($view -notmatch 'nonsense') 'and takes nothing from a driver log line that repeats its words'
    Check ($record -match 'FAILED in DisplayMapFramebuffer') 'and the record is picked out of the same output'
    Check ($record -notmatch 'start record log:' -and $record -notmatch 'nonsense') 'the summary line, not one of the log lines'
    Check ($health -match 'no reading: start health read refused') 'the health reading is the probe''s own line'
    Check ($health -notmatch 'flags 0') 'not a driver log line that happens to hold the same words'
    $detail = "$view; $record"
    Check ($detail -match 'device problem 43' -and $detail -match '0xC0000022') "verify's one line now carries both"

    '7. shared-state record protocol does not collapse errors into absence'
    $sharedRunner={param($exe,$arguments) @{code=0;text='{"abi":1,"error":0,"items":[{"path":"StartRecord","name":"Result","kind":"record","record":{"status":"ok","type":1,"bytes":16,"value":"failed"}},{"path":"StartRecord","name":"Status","kind":"record","record":{"status":"ok","type":4,"bytes":4,"value":3221225506}}]}'}}
    $view=Open-DriverStateRecordView -Path StartRecord -Cli fixture.exe -Runner $sharedRunner
    $sharedRecord=Read-StartRecordFromKey $view
    Check ($sharedRecord.Result -eq 'failed' -and $sharedRecord.Status -eq 3221225506) 'shared-state typed record reaches the existing formatter'
    $deniedRunner={param($exe,$arguments) @{code=0;text='{"abi":1,"error":0,"items":[{"path":"StartRecord","name":"Result","kind":"record","record":{"status":"access","os_error":5}}]}'}}
    $denied=$false;try{Open-DriverStateRecordView -Path StartRecord -Cli fixture.exe -Runner $deniedRunner}catch{$denied=$true}
    Check $denied 'unreadable shared record is an error, not an absent record'

    '8. the source rules both consumers must keep'
    $sc = [IO.File]::ReadAllText((Join-Path $Installer 'start-confirm.ps1'))
    Check ($sc -match 'function Read-StartRecord \{') 'start-confirm.ps1 reads the record'
    Check ($sc -match 'Open-DriverStateRecordView' -and $sc -notmatch 'Services\\bc250kmd\\Parameters') 'start-confirm reads only shared state'
    Check ($sc -match '(?m)^Write-StartRecordLog?$') 'and writes it into its log at every logon'
    Check (($sc -match 'start record: \$\(Format-StartRecordLine\)') -and ($sc -match '(?s)function Format-StartRecordLine \{\s+try \{ return \(Format-StartRecord -Record \(Read-StartRecord\) -BootUtc \$script:bootUtc\) \}\s+catch \{ return "unreadable: ')) 'and into its exit line'
    # A shared state that cannot be read is a log line, never the end of the run, and never "no record".
    Check ($sc -match '(?s)function Write-StartRecordLog\(\[int\]\$Lines = 24\) \{\s+try \{ \$record = Read-StartRecord \}\s+catch \{ Write-StartConfirmLog "start record: unreadable: ') 'an unreadable record is logged as unreadable and the run goes on'
    Check ($sc -match 'Write-StartRecordLog -Lines \$script:StartRecordMaxLogLines') 'the probe prints every kept line'
    Check ($sc -notmatch 'KeepLog') 'nothing about the record is behind the KeepLog gate'
    Check ($sc -match 'bootUtc = Get-StartRecordBootUtc') 'and it reads this boot''s start time for the record'
    Check (@([regex]::Matches($sc, '-BootUtc \$script:bootUtc')).Count -ge 3) 'which every reading of the record takes'
    # The load is guarded: this script runs under $ErrorActionPreference = 'Stop' and release-witness.ps1 runs
    # Add-Type at its top level, which needs a writable TEMP and the compiler. Unguarded, such a failure stopped the
    # whole task before it read anything, and -Probe printed no line at all for verify to parse.
    Check ($sc -match "(?m)^try \{ \. \(Join-Path \`$here 'release-witness\.ps1'\) \} catch \{") 'the task loads the boot identity before it reads the record, so -Probe has it too, and a failure of that load cannot stop it'
    Check ($sc -match 'release-witness\.ps1 did not load') 'and such a failure is a line in its log'
    $core = [IO.File]::ReadAllText((Join-Path $Installer 'start-confirm-core.ps1'))
    Check ($core -notmatch '(?m)^[^#\r\n]*TickCount') 'no code line reads a tick count any more (the comment that explains the field defect may name it)'
    Check ($core -match '(?m)^\s+if \(Get-Command -Name Get-BootIdentity .*\{ \$id = Get-BootIdentity \}') 'the boot identity comes from Get-BootIdentity, the one definition'
    Check ($core -match '(?m)^\s+try \{ \$utc = \[string\]\(Get-StartRecordBootIdentity\)\.boot_utc \}') 'the boot time is its boot_utc'
    # BD-130: this boot's BootId, which a stop record is matched against, comes from the page the driver writes
    # from, at the offset the WDK header fixes, and the registry copy is only the fallback.
    Check ($core -match '(?m)^\$script:SharedBootIdAddress = \[IntPtr\]0x7FFE02C4') 'this boot''s BootId is read at KUSER_SHARED_DATA + 0x2C4, the offset of the WDK header'
    Check ($core -match 'ntddk\.h' -and $core -match '9568' -and $core -match '9569') 'and the comment cites the header and the two C_ASSERTs that bracket the field'
    Check ($core -match '(?m)^\s+if \(\$null -ne \$shared\) \{ return \[ordered\]@\{ boot_id = \$shared; source = ''KUSER_SHARED_DATA'' \} \}') 'the page answers first, named as the source'
    Check ($core -match '(?m)^\s+try \{ \$id = \(Get-StartRecordBootIdentity\)\.boot_id \}') 'and the registry value of Get-BootIdentity stays as the fallback'
    Check ($core -match "(?m)^\s+\$\{?witness\}? = Join-Path \`$PSScriptRoot 'release-witness\.ps1'") 'and a caller that loads this file alone gets that definition from the script beside it'
    Check ($core -match '\$script:StopRecordKey = ''StopRecord''') 'the stop record key is named here, next to the start record key'
    Check ($core -match '(?m)^function Read-StopRecordFromKey') 'the stop record has its own reader, because its Result values are not the start record''s'
    Check ($core -match '(?m)^\s+foreach \(\$n in @\(''StartStage'', ''HealthFlags'', ''BootId'', ''LogLines''\)\)') 'which reads the four DWORDs of that record'
    Check ($core -match '(?m)^\s+foreach \(\$n in @\(''Generation'', ''Epoch'', ''StopTimeUtc''\)\) \{ \$r\[\$n\] = \[string\]') 'and keeps the generation, the epoch and the finish time as the strings the driver writes'
    $diag = [IO.File]::ReadAllText((Join-Path $Installer 'diagnostics.ps1'))
    Check ($diag -match 'Read-StopRecordFromKey' -and $diag -match 'Format-StopRecord') 'the diagnostics file reads the stop record through that reader'
    Check ($diag -match 'Test-StopRecordBoot -Record \$r -BootUtc \$BootUtc -BootId \$BootId') 'and places it in a boot before it shows it'
    Check ($diag -match '(?m)^#\s+writes no stop record at all when Windows stops it later') 'and its header says that a confirmed start leaves no stop record'
    $inst = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
    $lineRule = "\(\?m\)\^\(\?!\.\*start record log: \)\.\*"
    Check ($inst -match "probe -match '$lineRule\\bstart record: \(\.\+\)\`$'") 'the installer extracts the record from the probe, one whole line, never a driver log line'
    Check ($inst -match "probe -match '$lineRule" + "probe: fallback view: ") 'the view reading keeps the same rule'
    Check ($inst -match "probe -match '$lineRule" + "probe: start health ") 'and so does the health reading, which the record''s lines stand above'
    Check ($inst -match 'Add-Result .driver start. \$driverOk \$detail') "and prints it with verify's driver-start line"
    Check ($inst -match '\$driverOk = \(\$null -ne \$view\) -and \(\$view -match ''device problem 0,''\)') 'the verdict itself is unchanged'
} finally {
    $root.DeleteSubKeyTree($scratch, $false)
}
if ($Out) { [void][IO.Directory]::CreateDirectory($Out) }
"start record consumers: $(if ($fail) { "$fail FAILED" } else { 'all checks passed' }) (PowerShell $($PSVersionTable.PSVersion))"
exit $fail
