# BD-122: the consumer side of the KMD's start record (Parameters\StartRecord). The driver's own half is the host
# test driver\kmd\test\run_start_record.ps1; this one holds the readers and the words they print:
#
#   powershell -NoProfile -File tools\release\test-start-record.ps1 [-Installer <package>\installer] [-Out <dir>]
#
# Four fixtures, written as real registry values under a scratch key of HKCU (removed at the end) and read back
# through the one reader that start-confirm.ps1 and the installer use:
#   a step failure (the GitHub #38 shape: Code 43, LastStage 91, now with the step and the NTSTATUS),
#   the boot-loop guard's refusal,
#   a start that succeeded after a failure (the stale reason must not read as the current one),
#   no record at all (a KMD before 0.7.216.30).
# Then the extraction the installer's verify step performs on the probe's output, and the source rules that keep
# both consumers honest. Runs under Windows PowerShell 5.1 and PowerShell 7; nothing outside HKCU is touched.
param([string]$Installer = (Join-Path (Split-Path $PSScriptRoot) 'release\installer'), [string]$Out = '')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $Installer 'start-confirm-core.ps1'))) {
    $Installer = Join-Path $PSScriptRoot 'installer'
}
. (Join-Path $Installer 'start-confirm-core.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

$scratch = 'Software\amdgpu-wddm-start-record-test'
$root = [Microsoft.Win32.Registry]::CurrentUser
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
    Check ($t -match 'driver 0x000700D8') 'and the driver that wrote it, so a record from before an upgrade shows'
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
    Check ($t -match 'UnconfirmedStarts 1' -and $t -match 'driver 0x000700D8') 'the count and the driver are there too'
    Check (-not (Test-StartRecordIsReason $r)) 'and is not a reason'
    Check (@(Get-StartRecordLogLines -Record $r).Count -eq 0) 'the failure lines are gone with it'

    '4b. a record the previous boot left behind (the one case the driver''s rule cannot cover)'
    $boot = Get-StartRecordBootUtc
    Check ($null -ne $boot -and $boot -lt [datetime]::UtcNow) 'this boot''s start time is read from the tick count'
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

    '7. the source rules both consumers must keep'
    $sc = [IO.File]::ReadAllText((Join-Path $Installer 'start-confirm.ps1'))
    Check ($sc -match 'function Read-StartRecord \{') 'start-confirm.ps1 reads the record'
    Check ($sc -match '(?m)^Write-StartRecordLog?$') 'and writes it into its log at every logon'
    Check ($sc -match 'start record: \$\(Format-StartRecord -Record \(Read-StartRecord\) -BootUtc \$script:bootUtc\)') 'and into its exit line'
    Check ($sc -match 'Write-StartRecordLog -Lines \$script:StartRecordMaxLogLines') 'the probe prints every kept line'
    Check ($sc -notmatch 'KeepLog') 'nothing about the record is behind the KeepLog gate'
    Check ($sc -match 'bootUtc = Get-StartRecordBootUtc') 'and it reads this boot''s start time for the record'
    Check (@([regex]::Matches($sc, '-BootUtc \$script:bootUtc')).Count -ge 3) 'which every reading of the record takes'
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
