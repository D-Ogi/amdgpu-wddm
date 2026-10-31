# The installer's automatic diagnostics of a driver start that did not pass (diagnostics.ps1):
#
#   powershell -NoProfile -File tools\release\test-diagnostics.ps1 [-Installer <package>\installer] [-WorkRoot <dir>]
#
# The decision is a pure function, so the field report 20261011T222253Z is replayed as a fixture: its own readings
# (problem code CM_PROB_FAILED_POST_START, status Error, the start record of that boot, no start-health reading,
# LastStage 79) must come out as "start failure, collect". A healthy reading of the same shape must come out as
# "no start failure". Then the readers: the setupapi.dev.log section picker, the driver's log folder, the size and
# time bounds, and the whole file against an in-memory record store.
#
# Only -WorkRoot files are written. Registry, Windows metadata and events are synthetic.
param(
    [string]$Installer = (Join-Path (Split-Path $PSScriptRoot) 'release\installer'),
    [string]$WorkRoot = (Join-Path ([IO.Path]::GetTempPath()) ('amdgpu-wddm-diagnostics-' + [guid]::NewGuid().ToString('N').Substring(0, 8)))
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $Installer 'diagnostics.ps1'))) { $Installer = Join-Path $PSScriptRoot 'installer' }
. (Join-Path $Installer 'start-confirm-core.ps1')
. (Join-Path $Installer 'diagnostics.ps1')
. (Join-Path $Installer 'state-reader.ps1')
. (Join-Path $PSScriptRoot 'test-record-store.ps1')
$PSDefaultParameterValues['Write-StartFailureDiagnostics:EventReader']={{param($query,$limit)
    [pscustomobject]@{TimeCreated=[datetime]'2026-10-11T22:20:00Z';Level=2;Id=42;Message='synthetic driver event'}}}
$PSDefaultParameterValues['Write-StartFailureDiagnostics:WindowsReader']={{'Windows fixture; no host metadata read'}}
$script:HostReadAttempts=0
function Get-WinEvent {$script:HostReadAttempts++;throw 'host event access is forbidden in this fixture'}
function Get-DiagnosticsWindowsLines {$script:HostReadAttempts++;throw 'host Windows metadata access is forbidden in this fixture'}
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

# The scratch key has a suffix of its own, so two dry runs at the same time (the two flavours of a package run)
# never write, read back or remove each other's key.
$scratch = 'Software\amdgpu-wddm-diagnostics-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$parameters = "$scratch\Parameters"
$root = New-RecordFixtureKey
$root.DeleteSubKeyTree($scratch, $false)
[void][IO.Directory]::CreateDirectory($WorkRoot)
function New-RecordKey([string]$Name, [hashtable]$Values) {
    $k = $root.CreateSubKey("$parameters\$Name")
    try {
        foreach ($n in $Values.Keys) {
            $v = $Values[$n]
            if ($v -is [string[]]) { $k.SetValue($n, $v, [Microsoft.Win32.RegistryValueKind]::MultiString) }
            elseif ($v -is [int]) { $k.SetValue($n, $v, [Microsoft.Win32.RegistryValueKind]::DWord) }
            else { $k.SetValue($n, [string]$v, [Microsoft.Win32.RegistryValueKind]::String) }
        }
    } finally { $k.Dispose() }
}
function Get-Utc([string]$Text) {
    $styles = [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor [System.Globalization.DateTimeStyles]::AssumeUniversal
    return [datetime]::Parse($Text, [cultureinfo]::InvariantCulture, $styles)
}

try {
    # ---- the field case -----------------------------------------------------------------------------------------
    # run-1-result.json of 20261011T222253Z: boot_utc 22:19:45.5Z, and this detail, verbatim:
    $fieldDetail = 'driver bound: service bc250kmd, status Error, problem code CM_PROB_FAILED_POST_START | ' +
        'driver start: device problem 43, driver version  (expected 0x000700D8), LastStage 79; the last start reached a started device; ' +
        'stage 39, at 2026-10-11T22:19:52.317Z, UnconfirmedStarts 1, driver 0x000700D8 | ' +
        'start health: no reading: start health read refused, status 0xC000000E (15 = confirmed; 7 = full, ready, visible)'
    $fieldBoot = Get-Utc '2026-10-11T22:19:45.5000000Z'
    # The readings, taken out of that one line the way verify holds them.
    $fieldStatus = ([regex]::Match($fieldDetail, 'status (\w+),')).Groups[1].Value
    $fieldProblem = ([regex]::Match($fieldDetail, 'problem code (\S+)')).Groups[1].Value
    $fieldStage = [int]([regex]::Match($fieldDetail, 'LastStage (\d+)')).Groups[1].Value
    $fieldRecordTime = ([regex]::Match($fieldDetail, 'at (\S+?),')).Groups[1].Value
    New-RecordKey 'StartRecord' @{
        Result = 'started'; Step = '-'; SubStep = '-'; TimeUtc = $fieldRecordTime
        Status = [int]0; Stage = [int]39; UnconfirmedStarts = [int]1; Version = [int]0x000700D8
    }
    # The stop record the KMD of this train writes (guard.c GuardRecordStopBegin and GuardRecordStopDone): the
    # shared values with the start record's registry types, plus StartStage, HealthFlags, BootId and LogLines as
    # REG_DWORD and Generation, Epoch and StopTimeUtc as REG_SZ. BootId 11 is this boot's in these tests.
    New-RecordKey 'StopRecord' @{
        Result = 'stopped'; Step = 'StopDevice'; SubStep = 'unconfirmed start'; TimeUtc = '2026-10-11T22:19:53.100Z'
        Detail = 'Windows stopped the device; StopDevice supplies no reason code.'
        Status = [int]0; Stage = [int]79; StartStage = [int]39; HealthFlags = [int]3; BootId = [int]11
        Generation = '18446744073709551610'; Epoch = '4294967297'; StopTimeUtc = '2026-10-11T22:19:53.480Z'
        LogLines = [int]2; Version = [int]0x000700D8
        Log = [string[]]@('stop: dxgkrnl asked for a stop after a start nobody confirmed', 'stop: done')
    }
    $k = $root.OpenSubKey("$parameters\StartRecord")
    try { $fieldRecord = Read-StartRecordFromKey $k } finally { $k.Dispose() }

    '1. the field case 20261011T222253Z: these readings mean "start failure, collect"'
    Check ($fieldStatus -eq 'Error' -and $fieldProblem -eq 'CM_PROB_FAILED_POST_START' -and $fieldStage -eq 79) 'the readings come out of the field report'
    Check ((ConvertTo-DiagnosticsProblemCode $fieldProblem) -eq 43) 'a problem code read as its CM_PROB name is the number 43'
    Check ((Format-DiagnosticsProblemCode 43) -eq '43 (CM_PROB_FAILED_POST_START)') 'and the file prints the number with the name'
    Check ((Format-DiagnosticsProblemCode 'CM_PROB_FAILED_POST_START') -eq '43 (CM_PROB_FAILED_POST_START)') 'from either shape'
    $d = Get-StartFailureDecision -ProblemCode $fieldProblem -DevicePresent $true -DeviceStatus $fieldStatus `
        -Record $fieldRecord -BootUtc $fieldBoot -HealthFlags $null -LastStage $fieldStage
    Check ($d.collect) 'the decision is to collect'
    Check ((@($d.reasons) -join ' | ') -match '43 \(CM_PROB_FAILED_POST_START\)') 'because the device reports problem code 43'
    Check ((@($d.reasons) -join ' | ') -match 'status is Error') 'and the device status is Error'
    Check ((@($d.reasons) -join ' | ') -match 'no start-health reading') 'and the driver answered no start health'
    Check ((@($d.reasons) -join ' | ') -match 'LastStage 79') 'and the device left the started stages'
    Check ((@($d.notes) -join ' | ') -match 'reached a started device') 'while the start record of that boot is only a note: it says the start succeeded'
    Check ((@($d.notes) -join ' | ') -notmatch 'before this boot') 'and it is not disowned as another boot''s (the field defect of the boot time)'

    '2. a started driver collects nothing'
    New-RecordKey 'StartRecordOk' @{
        Result = 'started'; Step = '-'; SubStep = '-'; TimeUtc = [datetime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        Status = [int]0; Stage = [int]39; UnconfirmedStarts = [int]0; Version = [int]0x000700D8
    }
    $k = $root.OpenSubKey("$parameters\StartRecordOk")
    try { $okRecord = Read-StartRecordFromKey $k } finally { $k.Dispose() }
    $good = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' -Record $okRecord `
        -BootUtc ([datetime]::UtcNow.AddMinutes(-5)) -HealthFlags 15 -LastStage 50
    Check (-not $good.collect) 'problem code 0, status OK, flags 15, LastStage 50 and a started record: nothing to collect'
    Check ($good.summary -match '^no start failure') 'and the summary says so'
    $soon = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' -Record $okRecord `
        -BootUtc ([datetime]::UtcNow.AddMinutes(-5)) -HealthFlags 7 -LastStage 50
    Check (-not $soon.collect) 'a healthy start that nothing has confirmed yet is not a failure either'
    $noRecord = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' -Record $null `
        -BootUtc ([datetime]::UtcNow.AddMinutes(-5)) -HealthFlags 15 -LastStage 50
    Check (-not $noRecord.collect) 'and a driver before 0.7.216.30, which keeps no start record at all, is not one'
    Check ((@($noRecord.notes) -join ' | ') -match 'kept no start record') 'the absent record is a note, not a reason'

    'BD-147: the successful named problem code follows the same path as numeric zero'
    if (-not ('DiagnosticsTest.Problem' -as [type])) { Add-Type 'namespace DiagnosticsTest { public enum Problem { CM_PROB_NONE = 0, CM_PROB_FAILED_POST_START = 43 } }' }
    foreach ($code in @(0, '0', 'CM_PROB_NONE', ' cm_prob_none ', [DiagnosticsTest.Problem]::CM_PROB_NONE)) {
        Check ((ConvertTo-DiagnosticsProblemCode $code) -eq 0) "successful problem code decoded: $code"
        $named = Get-StartFailureDecision -ProblemCode $code -DevicePresent $true -DeviceStatus 'OK' -Record $okRecord `
            -BootUtc ([datetime]::UtcNow.AddMinutes(-5)) -HealthFlags 15 -LastStage 50
        Check (-not $named.collect -and $named.summary.StartsWith('no start failure:')) "healthy $code does not collect failure diagnostics"
    }
    # Run the real callback wrapper with only its side effects replaced. A successful enum value must never
    # invoke the collector, publish a failure event or print the misleading BD-147 information line.
    $tokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Installer 'install.ps1'), [ref]$tokens, [ref]$parseErrors)
    $wrapper = $ast.Find({ param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst] -and $a.Name -eq 'Invoke-VerifyDiagnostics' }, $true)
    . ([scriptblock]::Create($wrapper.Extent.Text))
    & {
        $script:bd147Collected = 0; $script:bd147Events = 0; $script:bd147Info = @()
        $script:DryRunMode = $false; $script:StateDir = $WorkRoot; $script:ServiceName = 'bc250kmd'
        $script:HardwareIdPrefix = 'fixture'; $script:EngineDiagnostics = $null
        function Write-Log { param($Text) }
        function Write-EngineEvent { param($Type, $Data) $script:bd147Events++ }
        function Write-StartFailureDiagnostics {
            $script:bd147Collected++
            return @{ written = $true; bytes = 1; seconds = 0; sections = @(); problems = @(); path = 'fixture.txt'; cut = $false }
        }
        $info = { param($Name, $Text) $script:bd147Info += ($Name + ': ' + $Text) }
        foreach ($problem in @('CM_PROB_NONE', [DiagnosticsTest.Problem]::CM_PROB_NONE)) {
            Invoke-VerifyDiagnostics -DeviceState @{ present = $true; problem_code = $problem; status = 'OK'; health_flags = 15; last_stage = 50 } `
                -StartRecord $okRecord -BootUtc ([datetime]::UtcNow.AddMinutes(-5)) -BootId 11 -BootIdSource 'fixture' -Info $info
        }
        Check ($script:bd147Collected -eq 0 -and $script:bd147Events -eq 0 -and $script:bd147Info.Count -eq 0) 'the real wrapper emits no failure diagnostics for CM_PROB_NONE'
        Invoke-VerifyDiagnostics -DeviceState @{ present = $true; problem_code = 'CM_PROB_FAILED_POST_START'; status = 'Error'; health_flags = $null; last_stage = 79 } `
            -StartRecord $fieldRecord -BootUtc $fieldBoot -BootId 11 -BootIdSource 'fixture' -Info $info
        Check ($script:bd147Collected -eq 1 -and $script:bd147Events -eq 1 -and ($script:bd147Info -join ' ') -match 'the driver did not start') 'a genuine failed start still collects and announces the details'
    }
    '3. every hard signal on its own'
    $cases = @(
        @{ name = 'no device with our hardware id'; args = @{ DevicePresent = $false; ProblemCode = $null; DeviceStatus = $null; HealthFlags = 15; LastStage = 50 }; want = 'no graphics device' }
        @{ name = 'a problem code that cannot be read'; args = @{ DevicePresent = $true; ProblemCode = 'nonsense'; DeviceStatus = 'OK'; HealthFlags = 15; LastStage = 50 }; want = 'cannot be read' }
        @{ name = 'Code 39, the driver did not load'; args = @{ DevicePresent = $true; ProblemCode = 39; DeviceStatus = 'Error'; HealthFlags = 15; LastStage = 50 }; want = 'CM_PROB_DRIVER_FAILED_LOAD' }
        @{ name = 'a start that stopped before the first mode set'; args = @{ DevicePresent = $true; ProblemCode = 0; DeviceStatus = 'OK'; HealthFlags = 15; LastStage = 30 }; want = 'before the first mode set' }
        @{ name = 'a start that is not full, ready and visible'; args = @{ DevicePresent = $true; ProblemCode = 0; DeviceStatus = 'OK'; HealthFlags = 1; LastStage = 50 }; want = 'not full, ready and visible' }
    )
    foreach ($c in $cases) {
        $args1 = $c.args
        $one = Get-StartFailureDecision -Record $okRecord -BootUtc ([datetime]::UtcNow.AddMinutes(-5)) @args1
        Check ($one.collect -and ((@($one.reasons) -join ' | ') -match [regex]::Escape($c.want))) "$($c.name): collect, and the reason names it"
    }
    foreach ($result in @('failed', 'guard', 'attempt')) {
        New-RecordKey "StartRecord-$result" @{
            Result = $result; Step = $(if ($result -eq 'failed') { 'SmuOwnerStart' } else { '-' }); SubStep = '-'
            TimeUtc = [datetime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
            Status = [int]-1073741790; Stage = [int]91; UnconfirmedStarts = [int]2; Version = [int]0x000700D8
        }
        $k = $root.OpenSubKey("$parameters\StartRecord-$result")
        try { $r = Read-StartRecordFromKey $k } finally { $k.Dispose() }
        $one = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' -Record $r `
            -BootUtc ([datetime]::UtcNow.AddMinutes(-5)) -HealthFlags 15 -LastStage 50
        Check ($one.collect) "a start record of this boot with result '$result' is a start failure on its own"
    }
    $stale = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' `
        -Record (& { $k = $root.OpenSubKey("$parameters\StartRecord"); try { Read-StartRecordFromKey $k } finally { $k.Dispose() } }) `
        -BootUtc ([datetime]::UtcNow) -HealthFlags 15 -LastStage 50
    Check (-not $stale.collect -and ((@($stale.notes) -join ' | ') -match 'before this boot')) 'a record an earlier boot left decides nothing'

    '3b. a boot time that cannot be read decides nothing either'
    # Get-StartRecordBootUtc returns $null when no definition of this boot answers (no CIM). Nothing can tell then
    # whether a record is this boot's, so a failed record of an EARLIER boot must not be read as this start's
    # reason: a healthy install would collect a file and blame a step of another boot, with no note to say so.
    New-RecordKey 'StartRecordOld' @{
        Result = 'failed'; Step = 'WddmStart'; SubStep = '-'; TimeUtc = '2026-09-21T04:44:03.5Z'
        Status = [int]-1073741790; Stage = [int]91; UnconfirmedStarts = [int]2; Version = [int]0x000700D8
    }
    $k = $root.OpenSubKey("$parameters\StartRecordOld")
    try { $oldRecord = Read-StartRecordFromKey $k } finally { $k.Dispose() }
    $unknown = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' -Record $oldRecord `
        -BootUtc $null -HealthFlags 15 -LastStage 50
    Check (-not $unknown.collect) 'a healthy install on a PC whose boot time cannot be read collects nothing'
    Check ((@($unknown.notes) -join ' | ') -match 'boot time of this computer is unknown') 'and the note says the boot time is unknown'
    Check ((@($unknown.reasons) -join ' | ') -notmatch 'WddmStart') 'the step of the older record is no reason'
    $unknownBad = Get-StartFailureDecision -ProblemCode 43 -DevicePresent $true -DeviceStatus 'Error' -Record $oldRecord `
        -BootUtc $null -HealthFlags $null -LastStage 79
    Check ($unknownBad.collect -and ((@($unknownBad.reasons) -join ' | ') -match 'CM_PROB_FAILED_POST_START')) 'while the readings that need no boot time still decide a failure'

    '4. the setupapi.dev.log sections of this device'
    $log = Join-Path $WorkRoot 'setupapi.dev.log'
    $body = New-Object Collections.ArrayList
    foreach ($n in 1..7) {
        [void]$body.Add(">>>  [Device Install (Hardware initiated) - PCI\VEN_1002&DEV_13FE&SUBSYS_13FE1002&REV_C1\4&2b8b7e0&0&0008]")
        [void]$body.Add(">>>  Section start 2026/10/11 22:1$n`:00.000")
        [void]$body.Add("     dvi: section $n of ours")
        [void]$body.Add("<<<  Section end 2026/10/11 22:1$n`:01.000")
        [void]$body.Add(">>>  [Device Install (Hardware initiated) - PCI\VEN_8086&DEV_0000\other]")
        [void]$body.Add("     dvi: a foreign device")
        [void]$body.Add("<<<  Section end 2026/10/11 22:1$n`:02.000")
    }
    [IO.File]::WriteAllLines($log, [string[]]@($body))
    $sel = Select-DiagnosticsSetupApiSections -Path $log -HardwareId 'PCI\VEN_1002&DEV_13FE' -MaxSections 3
    Check ($sel.seen -eq 7) 'every section whose header names the device is seen'
    Check (@($sel.sections).Count -eq 3) 'and only the newest three are kept'
    Check ((@($sel.sections)[-1] -join ' ') -match 'section 7 of ours') 'the last one kept is the newest in the file'
    Check (-not ((@($sel.sections) | ForEach-Object { $_ -join ' ' }) -join ' ') -or ((@($sel.sections) | ForEach-Object { $_ -join ' ' }) -join ' ') -notmatch 'foreign device') 'and no line of a foreign device comes with them'
    $cut = Select-DiagnosticsSetupApiSections -Path $log -HardwareId 'PCI\VEN_1002&DEV_13FE' -MaxSections 2 -MaxLines 2
    Check (((@($cut.sections) | ForEach-Object { $_ -join ' ' }) -join ' ') -match 'longer than 2 lines') 'a long section says where it was cut'
    $none = Select-DiagnosticsSetupApiSections -Path (Join-Path $WorkRoot 'no-such.log') -HardwareId 'PCI\VEN_1002&DEV_13FE'
    Check (@($none.sections).Count -eq 0 -and $none.note -match 'is not there') 'a log that is not there is a note, not a failure'

    '5. the driver''s own log folder'
    $kmdlog = Join-Path $WorkRoot 'kmdlog'
    [void][IO.Directory]::CreateDirectory($kmdlog)
    foreach ($n in 1..5) {
        $f = Join-Path $kmdlog "bc250kmd-$n.log"
        [IO.File]::WriteAllLines($f, [string[]]@(1..50 | ForEach-Object { "line $_ of file $n" }))
        (Get-Item -LiteralPath $f).LastWriteTimeUtc = [datetime]::UtcNow.AddMinutes(-$n)
    }
    $kl = @(Get-DiagnosticsKmdLogLines -Directory $kmdlog -MaxFiles 2 -MaxLines 5)
    Check (($kl -join ' ') -match 'bc250kmd-1\.log') 'the newest file is there'
    Check (($kl -join ' ') -notmatch 'bc250kmd-3\.log') 'and only the newest two'
    Check (@($kl | Where-Object { $_ -match 'line 50 of file 1' }).Count -eq 1) 'with the last lines of each one'
    Check (@($kl | Where-Object { $_ -match 'line 46 of file 1' }).Count -eq 1 -and @($kl | Where-Object { $_ -match 'line 45 of file 1' }).Count -eq 0) 'exactly the last five'
    Check ((@(Get-DiagnosticsKmdLogLines -Directory (Join-Path $WorkRoot 'nothing')) -join ' ') -match 'is not there') 'a folder that is not there is a line, not a failure'

    '6. the bounds'
    $sink = New-DiagnosticsSink -MaxBytes 200
    $taken = 0
    foreach ($n in 1..100) { if (Add-DiagnosticsLine -Sink $sink -Text ('x' * 20) -Where 'test') { $taken++ } }
    Check ($taken -gt 0 -and $taken -lt 100) 'the size bound stops the file in the middle of a section'
    Check ($sink.cut -and $sink.text.ToString() -match 'bound was reached') 'and the file says where it was cut'
    Check ($sink.bytes -le 200 + 100) 'and nothing much more than the bound is held'
    # The bound is bytes of the file, not characters of the text: the file is UTF-8, and every line of it comes from
    # the OS in the OS's own language (event messages, setupapi text, the driver's log). A character count let a
    # file that reported its 4096 KiB hold about three times that on disk on a ja-JP or ko-KR installation.
    $wide = New-DiagnosticsSink -MaxBytes 1024
    foreach ($n in 1..200) { [void](Add-DiagnosticsLine -Sink $wide -Text ([string][char]0x3053 * 40) -Where 'wide') }
    $wideReal = [Text.Encoding]::UTF8.GetByteCount($wide.text.ToString())
    Check ($wideReal -le 1024 + 200) "the bound counts UTF-8 bytes: 1 KiB of three-byte characters is $wideReal bytes on disk"
    Check ($wide.bytes -ge $wideReal - 200 -and $wide.bytes -le $wideReal + 200) "and the count the last line reports is that number ($($wide.bytes))"

    '7. the whole file, from the records of the scratch key'
    $state = [ordered]@{
        present = $true; instance_id = 'PCI\VEN_1002&DEV_13FE&SUBSYS_13FE1002&REV_C1\4&2b8b7e0&0&0008'
        problem_code = $fieldProblem; status = $fieldStatus; service = 'bc250kmd'; driver_version = '0.7.216.100'
        inf = 'oem42.inf'; provider = 'amdgpu-wddm'; package_version = '0.7.216.100'; reply_version = $null
        last_stage = $fieldStage; health_flags = $null
    }
    $out = Join-Path $WorkRoot 'diagnostics.txt'
    $r = Write-StartFailureDiagnostics -Path $out -Decision $d -DeviceState $state -BootUtc $fieldBoot -BootId 11 `
        -Root $root -ParametersSubKey $parameters -SetupApiLog $log -KmdLogDir $kmdlog -Context 'host test' -BudgetSeconds 20
    Check ($r.written -and (Test-Path -LiteralPath $out)) 'the file is written'
    Check (@($r.problems).Count -eq 0) "and no section failed ($(@($r.problems) -join '; '))"
    Check ($r.seconds -le 20) "within the 20 s budget ($($r.seconds) s)"
    Check ($r.bytes -le 4 * 1024 * 1024 -and -not $r.cut) "and within the 4 MiB bound ($($r.bytes) bytes)"
    $text = [IO.File]::ReadAllText($out)
    Check ($text -match 'schema amdgpu-wddm\.diagnostics/1') 'it names its schema'
    Check ($text -match 'start failure: .*43 \(CM_PROB_FAILED_POST_START\)') 'the decision and its reasons are at the top'
    Check ($text -match '(?m)^StartRecord : the last start reached a started device') 'the start record is read through the one reader'
    Check ($text -notmatch 'not about any start of this boot') 'and is not disowned: the boot time is this boot''s'
    Check ($text -match '(?m)^StopRecord : Windows stopped the device after a start that nothing confirmed, and the teardown finished') 'the stop record is read through its own reader, which knows what its Result values mean'
    Check ($text -match '(?m)^StopRecord : .*stage 39 before the stop, stop stage 79') 'the line names the stage before the stop and the stage the stop reached'
    Check ($text -match '(?m)^StopRecord values: .*StartStage 39, HealthFlags 3, BootId 11') 'the values section prints the DWORDs of that record'
    Check ($text -match "(?m)^StopRecord values: .*Generation '18446744073709551610', Epoch '4294967297'") 'and the generation and the epoch with all their digits'
    Check ($text -match "(?m)^StopRecord values: .*StopTimeUtc '2026-10-11T22:19:53.480Z', LogLines 2") 'and the teardown''s finish time and its line count'
    Check ($text -match '(?m)^StopRecord status: .*which is no verdict: StopDevice gives the driver no code') 'Status 0 is never shown as a success'
    Check ($text -match '(?m)^StopRecord boot: this record this boot; the record''s BootId is 11, this boot''s is 11') 'and the record is placed in a boot before it is shown'
    Check ($text -notmatch '(?m)^StopRecord : .*EARLIER boot') 'a record of this boot carries no note about another one'
    Check ($text -match '(?m)^StopRecord log: stop: done') 'with its kept lines'
    Check ($text -match 'device problem code: 43 \(CM_PROB_FAILED_POST_START\)') 'the device state carries the code with its name'
    Check ($text -match 'device status: Error' -and $text -match 'driver INF: oem42\.inf' -and $text -match 'device service: bc250kmd') 'and the status, the INF and the service'
    Check ($text -match '== the event logs of this boot ==') 'the event-log section is there'
    Check ($text -match '(?m)^Microsoft-Windows-DxgKrnl in Microsoft-Windows-DxgKrnl-Admin : ') 'with a line for each source, read or not, and DxgKrnl read from its own channel'
    Check ($text -notmatch '(?m)^Microsoft-Windows-DxgKrnl in System : ') 'and never from the System log, where that provider writes nothing'
    Check ($text -match 'section 7 of ours' -and $text -notmatch 'a foreign device') 'the setup log sections are this device''s only'
    Check ($text -match 'line 50 of file 1') 'the driver''s own log files are there'
    Check ($text -match '(?m)^section the stop record : ') 'and the file ends with what each section gave'
    Check ($text -match 'took \d+\.\d s of the 20 s budget; \d+ of 4096 KiB') 'and with the time and the size it used'

    '7a. the same stop record, read in a LATER boot: a note, and nothing else changes'
    # A stop of a start that WAS confirmed writes no record at all, so the record of an unconfirmed start survives
    # every healthy install after it. Read in a later boot the same key must be shown as that earlier boot's; the
    # file still prints every value, because the detail belongs in a problem report either way.
    $outLater = Join-Path $WorkRoot 'diagnostics-later-boot.txt'
    $rLater = Write-StartFailureDiagnostics -Path $outLater -Decision $d -DeviceState $state -BootUtc ([datetime]::UtcNow) -BootId 12 `
        -Root $root -ParametersSubKey $parameters -SetupApiLog $log -KmdLogDir $kmdlog -Context 'host test' -BudgetSeconds 20
    $textLater = [IO.File]::ReadAllText($outLater)
    Check ($rLater.written -and @($rLater.problems).Count -eq 0) "the file is written as before ($(@($rLater.problems) -join '; '))"
    Check ($textLater -match '(?m)^StopRecord : .*NOTE this record is from an EARLIER boot') 'the line says the record is from an earlier boot'
    Check ($textLater -match '(?m)^StopRecord boot: this record earlier boot; the record''s BootId is 11, this boot''s is 12') 'and names both BootId values'
    Check ($textLater -match "(?m)^StopRecord values: .*Generation '18446744073709551610'") 'every value is still printed, because the detail belongs in the report'
    Check ($textLater -match '(?m)^StopRecord log: stop: done') 'and so are the kept lines'
    Check ($textLater -match 'this boot started .*BootId 12') 'the head of the file names this boot''s own BootId'
    # A computer whose BootId cannot be read at all: the record is placed by its time, and when that cannot answer
    # either the file says so rather than claiming this boot.
    $outNoId = Join-Path $WorkRoot 'diagnostics-no-boot-id.txt'
    $rNoId = Write-StartFailureDiagnostics -Path $outNoId -Decision $d -DeviceState $state -BootUtc $null -BootId $null `
        -Root $root -ParametersSubKey $parameters -SetupApiLog (Join-Path $WorkRoot 'no-such.log') `
        -KmdLogDir (Join-Path $WorkRoot 'nothing') -EventSources @() -BudgetSeconds 10
    $textNoId = [IO.File]::ReadAllText($outNoId)
    Check ($rNoId.written) 'a computer that answers neither a boot time nor a BootId still gets its file'
    Check ($textNoId -match '(?m)^StopRecord : .*NOTE neither the BootId nor the boot time') 'and the stop record is not claimed for this boot'
    Check ($textNoId -match '(?m)^StopRecord boot: this record unknown; .*this boot''s cannot be read') 'the boot line says which half is missing'
    Check ($textNoId -match 'this boot started at a time that cannot be read, BootId cannot be read') 'and so does the head of the file'

    '7a2. BD-130: the file says where this boot''s BootId was read, and a higher number is no earlier boot'
    # The driver writes KUSER_SHARED_DATA.BootId into the record. This boot's number came from the registry copy of
    # that counter, which lags inside a boot (the lab, 22631: page 388, registry 387), so the FRESH record of this
    # boot was reported as an earlier boot's. The number now comes from the page, and the file names the source, so
    # a report that was collected on the weaker source says so itself.
    $outSource = Join-Path $WorkRoot 'diagnostics-boot-source.txt'
    $rSource = Write-StartFailureDiagnostics -Path $outSource -Decision $d -DeviceState $state -BootUtc $fieldBoot -BootId 11 `
        -BootIdSource 'KUSER_SHARED_DATA' -Root $root -ParametersSubKey $parameters -SetupApiLog $log -KmdLogDir $kmdlog `
        -Context 'host test' -BudgetSeconds 20
    $textSource = [IO.File]::ReadAllText($outSource)
    Check ($rSource.written -and @($rSource.problems).Count -eq 0) "the file is written as before ($(@($rSource.problems) -join '; '))"
    Check ($textSource -match 'this boot started .*BootId 11 \(read from KUSER_SHARED_DATA\)') 'the head of the file names the source of this boot''s number'
    Check ($textSource -match '(?m)^StopRecord boot: this record this boot; .*this boot''s is 11, read from KUSER_SHARED_DATA') 'and the source stands beside the comparison itself'
    # The same record against the lagging registry number: a verdict that says what the two numbers mean.
    $outLag = Join-Path $WorkRoot 'diagnostics-lagging-registry.txt'
    $lagRecord = [ordered]@{ Result = 'stopped'; Step = 'StopDevice'; SubStep = 'unconfirmed start'; Stage = [int]79
        StartStage = [int]50; HealthFlags = [int]3; BootId = [int]388; LogLines = [int]1 }
    $lagKey = $root.CreateSubKey("$parameters\StopRecord")
    try { foreach ($n in $lagRecord.Keys) { $lagKey.SetValue($n, $lagRecord[$n]) } } finally { $lagKey.Dispose() }
    $rLag = Write-StartFailureDiagnostics -Path $outLag -Decision $d -DeviceState $state -BootUtc $fieldBoot -BootId 387 `
        -BootIdSource 'PrefetchParameters registry' -Root $root -ParametersSubKey $parameters -SetupApiLog $log `
        -KmdLogDir $kmdlog -Context 'host test' -BudgetSeconds 20
    $textLag = [IO.File]::ReadAllText($outLag)
    Check ($rLag.written) 'the file of such a reading is written too'
    Check ($textLag -match '(?m)^StopRecord boot: this record later boot; the record''s BootId is 388, this boot''s is 387, read from PrefetchParameters registry') 'a record whose BootId is higher is placed as a later boot, with the weaker source named'
    Check ($textLag -match "(?m)^StopRecord : .*the record's BootId is HIGHER than the number read for this boot") 'and the line says the two numbers come from two counters'
    Check ($textLag -cnotmatch '(?m)^StopRecord : .*from an EARLIER boot') 'never the sentence of the field defect'

    '7b. an absent stop record, an absent log and an absent folder change nothing'
    $root.DeleteSubKeyTree("$parameters\StopRecord", $false)
    $out2 = Join-Path $WorkRoot 'diagnostics-2.txt'
    $r2 = Write-StartFailureDiagnostics -Path $out2 -Decision $d -DeviceState $state -BootUtc $fieldBoot `
        -Root $root -ParametersSubKey $parameters -SetupApiLog (Join-Path $WorkRoot 'no-such.log') -KmdLogDir (Join-Path $WorkRoot 'nothing') -BudgetSeconds 20
    $text2 = [IO.File]::ReadAllText($out2)
    Check ($r2.written -and @($r2.problems).Count -eq 0) 'the file is still written, and nothing counts as a problem'
    Check ($text2 -match '(?m)^StopRecord : absent') 'an absent stop record says so, which is the normal case'
    Check ($text2 -match 'is not there') 'and so do the log and the folder'

    '7c. the time budget and the size bound end the collection without a throw'
    $out3 = Join-Path $WorkRoot 'diagnostics-3.txt'
    $r3 = Write-StartFailureDiagnostics -Path $out3 -Decision $d -DeviceState $state -BootUtc $fieldBoot `
        -Root $root -ParametersSubKey $parameters -SetupApiLog $log -KmdLogDir $kmdlog -BudgetSeconds 0
    $text3 = [IO.File]::ReadAllText($out3)
    Check ($r3.written) 'a budget of 0 s still writes the file'
    Check ($text3 -match 'budget ran out before this section') 'and every section says the budget ran out'
    Check (@($r3.sections | Where-Object { $_ -match 'not collected' }).Count -ge 5) 'and the summary names them'
    $out4 = Join-Path $WorkRoot 'diagnostics-4.txt'
    $r4 = Write-StartFailureDiagnostics -Path $out4 -Decision $d -DeviceState $state -BootUtc $fieldBoot `
        -Root $root -ParametersSubKey $parameters -SetupApiLog $log -KmdLogDir $kmdlog -MaxBytes 1024
    Check ($r4.written -and $r4.cut -and $r4.bytes -le 1024 + 200) 'a 1 KiB bound cuts the file and writes it anyway'
    $r5 = Write-StartFailureDiagnostics -Path (Join-Path $WorkRoot "no`0name.txt") -Decision $d -DeviceState $state -BootUtc $fieldBoot `
        -Root $root -ParametersSubKey $parameters -SetupApiLog $log -KmdLogDir $kmdlog
    Check (-not $r5.written -and @($r5.problems).Count -ge 1 -and $null -eq $r5.path) 'a path that cannot be written comes back as a problem, not as a throw'
    # The write stages the text next to the file and moves it into place. A move or a replace that fails must leave
    # no staging file behind: the production path is the installer's state folder, and three gates of this tree
    # compare the files of that folder before and after themselves.
    $orphanDir = Join-Path $WorkRoot 'orphan'
    [void][IO.Directory]::CreateDirectory($orphanDir)
    $blocked = Join-Path $orphanDir 'taken'
    [void][IO.Directory]::CreateDirectory($blocked)      # a directory cannot be replaced by a file
    $r6 = Write-StartFailureDiagnostics -Path $blocked -Decision $d -DeviceState $state -BootUtc $fieldBoot `
        -Root $root -ParametersSubKey $parameters -SetupApiLog (Join-Path $WorkRoot 'no-such.log') `
        -KmdLogDir (Join-Path $WorkRoot 'nothing') -EventSources @() -BudgetSeconds 5
    $left = @(Get-ChildItem -LiteralPath $orphanDir -Force -File -ErrorAction SilentlyContinue)
    Check (-not $r6.written -and @($r6.problems).Count -ge 1) 'a target that cannot be replaced is a problem, not a throw'
    Check (@($left).Count -eq 0) "and no staging file is left beside it ($(@($left | ForEach-Object { $_.Name }) -join ', '))"

    '7d. every event source names a log its provider writes to'
    # A Get-WinEvent query that names a log the provider does not write to throws on every call, so such a row can
    # never return an entry: Microsoft-Windows-DxgKrnl has no System channel at all (it writes to its own -Admin and
    # -Operational logs), which made the one section meant to carry the GPU's own voice read "not read" on every PC.
    foreach ($s in @($script:DiagnosticsEventSources)) {
        $sp = [string]$s.provider
        $sl = [string]$s.log
        Check ([bool]$sl) "$sp names the log it is read from"
        $links = if($sp -eq 'Microsoft-Windows-DxgKrnl'){@('Microsoft-Windows-DxgKrnl-Admin','Microsoft-Windows-DxgKrnl-Operational')}else{@('System')}
        Check ($links -contains $sl) "$sp selects its declared fixture channel $sl"
    }

    '8. shared-state diagnostics retain typed failures'
    $sharedRunner={param($exe,$arguments) @{code=0;text='{"abi":1,"error":0,"items":[{"path":"StartRecord","name":"Result","kind":"record","record":{"status":"ok","type":1,"bytes":16,"value":"failed"}}]}'}}
    $lines=Get-DiagnosticsRecordLines -Root $root -ParametersSubKey '' -SubKey StartRecord -StartRecord -StateRunner $sharedRunner
    Check (($lines -join "`n") -match 'FAILED') 'production diagnostic reader uses the shared protocol'
    $deniedRunner={param($exe,$arguments) @{code=5;text='{"abi":1,"error":5,"items":[]}'}}
    $denied=$false;try{Get-DiagnosticsRecordLines -Root $root -ParametersSubKey '' -SubKey StartRecord -StartRecord -StateRunner $deniedRunner}catch{$denied=$true}
    Check $denied 'denied shared state cannot report an absent StartRecord'

    '9. the contract of the callers'
    Check ($script:HostReadAttempts -eq 0) 'every event and Windows metadata query used the injected fixture'
    $inst = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
    Check ($inst -match "(?m)^\. \(Join-Path \`$here 'diagnostics\.ps1'\)") 'install.ps1 loads the collector'
    Check (@([regex]::Matches($inst, 'Invoke-VerifyDiagnostics')).Count -ge 3) 'and calls it from verify, at the device check and after the start health'
    Check ($inst -match 'Add-Result .device. \$false .BC-250 GPU not found.[\s\S]{0,400}Invoke-VerifyDiagnostics') 'a GPU that is not there collects as well, before verify returns'
    Check ($inst -match '\$script:EngineDiagnostics = \$r\.path') 'the path of the file reaches the engine result'
    Check ($inst -match 'Write-EngineEvent .diagnostics.') 'and an event says that it was collected'
    Check ($inst -match 'if \(\$script:EngineDiagnostics\) \{[\s\S]{0,200}Send these two files') 'and the console route names it in its last lines'
    $eng = [IO.File]::ReadAllText((Join-Path $Installer 'engine.ps1'))
    Check ($eng -match '(?m)^\s+diagnostics = \$script:EngineDiagnostics\s*$') 'the terminal result carries the path (docs/gui/interfaces-setup.md section 3)'
} finally {
    $root.DeleteSubKeyTree($scratch, $false)
    Remove-Item -LiteralPath $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
}
"installer diagnostics: $(if ($fail) { "$fail FAILED" } else { 'all checks passed' }) (PowerShell $($PSVersionTable.PSVersion))"
exit $fail
