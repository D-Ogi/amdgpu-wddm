# Host test of the BD-389 cause reader (installer\compositor-cause.ps1) and of its two consumers: the decision of
# diagnostics.ps1 (Get-StartFailureDecision) and the cause line of the start-confirm task (start-confirm-core.ps1
# Get-StartConfirmCauseLine).
#
# The readings are the real ones of one lab unit (test-data\bd389\unit-a-20261124.txt): the rc3 driver set alone on
# Windows 11 26200 without the Visual C++ runtime. dwm.exe ended 8 times with 0x889800b2 in dwmcore.dll, Windows
# stopped the GPU use of the session and removed the device as failing (problem code 43), while the KMD's start
# record said 'started'. The four readings of the cause:
#   1. that shape gives the reason, and the imports of the driver set that this system does not have;
#   2. the same restarts with another primary display device give no reason;
#   3. the same events seen from a later boot give no reason;
#   4. imports that this system does not have, without DWM events, give a note and no reason.
# The PE import reader is checked against real images: it reads the import tables without loading the DLL, gives
# the names that tools/quality/pe_image.py gives (the imports block of the fixture, when BC250_ROOT holds the
# attempt 6 driver set), and fails closed on an image that it cannot judge.
#
#   powershell -NoProfile -File tools\release\test-compositor-cause.ps1 [-Installer <installer folder>]
#
# Runs under Windows PowerShell 5.1 and PowerShell 7. It reads no event log, no registry value and no device of the
# host. It reads system DLLs of the host as files, for the reader's positive control.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'),
      [string]$Fixture = (Join-Path $PSScriptRoot 'test-data\bd389\unit-a-20261124.txt'))
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'start-confirm-core.ps1')
. (Join-Path $Installer 'compositor-cause.ps1')
. (Join-Path $Installer 'diagnostics.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

# ---- the fixture ----------------------------------------------------------------------------------------------
# Events as the cause reader takes them (TimeCreated in UTC, LogName, ProviderName, Id, Message), the boot times,
# the two start records, the device, the runtime DLLs that the unit did not have, and the import tables.
function Read-Fixture([string]$Path) {
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $utc = [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
    $f = [ordered]@{ events = New-Object Collections.ArrayList; boots = New-Object Collections.ArrayList
        records = New-Object Collections.ArrayList; absent = New-Object Collections.ArrayList
        images = New-Object Collections.ArrayList; device_name = $null; problem = $null; instance_id = $null }
    $date = $null; $log = $null; $block = $null; $record = $null
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line.StartsWith('#')) { continue }
        if ($line -match '^date (\d{4}-\d{2}-\d{2})$') { $date = $Matches[1]; continue }
        if ($line.StartsWith('=== ')) { $block = $line; $log = $null; $record = $null; continue }
        if ($block -like '=== imports*') {
            if ($line -match '^(\S+) (\S+) import (\S+) delay (\S+)$') {
                $names = { param($t) if ($t -eq '-') { @() } else { @($t -split ',') } }
                [void]$f.images.Add([ordered]@{ path = $Matches[1]; machine = $Matches[2]; import = @(& $names $Matches[3]); delay = @(& $names $Matches[4]); error = $null })
            }
            continue
        }
        if ($line -match '(?:^|; )boot (\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)$') { [void]$f.boots.Add([datetime]::Parse($Matches[1], $inv, $utc)); continue }
        if ($line -match "^device '([^']+)' status (\S+) problem (\d+)") { $f.device_name = $Matches[1]; $f.problem = [int]$Matches[3]; continue }
        if ($line -match '^\s+(\S+\.dll) absent$') { [void]$f.absent.Add($Matches[1]); continue }
        if ($line -match '^--- record StartRecord') { $record = [ordered]@{}; [void]$f.records.Add($record); $log = $null; continue }
        if ($null -ne $record -and $line -match '^\s+(\w+) = (.*)$') { $record[$Matches[1]] = $Matches[2]; continue }
        if ($line -match '^\s+StartRecord\s+(\w+),') { $record = [ordered]@{ Result = $Matches[1] }; [void]$f.records.Add($record); continue }
        if ($null -ne $record -and $line -match ' at (\d{4}-\d{2}-\d{2}T[0-9:.]+Z),') { $record['TimeUtc'] = $Matches[1]; $record = $null; continue }
        if ($line -match '^--- Application log:') { $log = 'Application'; $record = $null; continue }
        if ($line -match '^--- log (.+?): \d+ events$') { $log = $Matches[1]; $record = $null; continue }
        if ($line.StartsWith('--- ')) { $log = $null; $record = $null; continue }
        if ($log -and $line -match '^\s+(\d{2}:\d{2}:\d{2})Z (.*?)\s*id (\d+) L(\d): (.*)$') {
            $when = [datetime]::Parse("$($date)T$($Matches[1])Z", $inv, $utc)
            [void]$f.events.Add([pscustomobject]@{ TimeCreated = $when; LogName = $log; ProviderName = $Matches[2]; Id = [int]$Matches[3]
                Level = [int]$Matches[4]; Message = $Matches[5] })
            if (-not $f.instance_id -and $Matches[5] -match '^Device (PCI\\VEN_1002&DEV_13FE\S+) ') { $f.instance_id = $Matches[1] }
        }
    }
    return $f
}
$f = Read-Fixture $Fixture
Check ($f.boots.Count -eq 2 -and $f.records.Count -eq 2 -and $f.events.Count -eq 27 -and $f.images.Count -eq 24) "fixture: 2 boots, 2 start records, $($f.events.Count) events, $($f.images.Count) images"
Check ($f.device_name -eq 'BC-250 GPU (amdgpu-wddm)' -and $f.problem -eq 43 -and $f.instance_id -like 'PCI\VEN_1002&DEV_13FE*') "fixture: the device '$($f.device_name)', problem $($f.problem), $($f.instance_id)"
Check ((@($f.absent) -join ',') -eq 'vcruntime140.dll,vcruntime140_1.dll,msvcp140.dll') "fixture: the unit had no $(@($f.absent) -join ', ')"
$boot1 = $f.boots[0]; $boot2 = $f.boots[1]; $rec1 = $f.records[0]; $rec2 = $f.records[1]
Check ($rec1.Result -eq 'started' -and $rec1.TimeUtc -eq '2026-11-24T12:58:18.484Z' -and $rec2.TimeUtc -eq '2026-11-24T13:08:28.385Z') 'fixture: the start records of both boots say started'

# The clean 26200 of the reading: every imported DLL is there but the three runtime DLLs that vc-install.ps1 found
# absent. The API sets are the loader's own (compositor-cause.ps1 never asks the check about them).
$absentNames = @($f.absent)
$present = { param($Dll, $Machine, $Directory) -not ($absentNames -contains $Dll.ToLowerInvariant()) }
$gaps = Find-AbsentImports -Images $f.images -Present $present
$want = @('desktop/amdgpu_wddm_radv.dll MSVCP140.dll', 'desktop/amdgpu_wddm_radv.dll VCRUNTIME140.dll', 'desktop/amdgpu_wddm_radv.dll VCRUNTIME140_1.dll',
          'desktop/bc250d3d_router.dll VCRUNTIME140.dll',
          'desktop/bc250d3d_zink.dll MSVCP140.dll', 'desktop/bc250d3d_zink.dll VCRUNTIME140.dll', 'desktop/bc250d3d_zink.dll VCRUNTIME140_1.dll')
$have = @(@($gaps.absent) | ForEach-Object { "$($_.file) $($_.dll)" } | Sort-Object)
Check (($have -join '|') -eq (($want | Sort-Object) -join '|')) "imports: the three desktop-route files lack 7 runtime imports, nothing else ($($have.Count))"
Check (@($gaps.unreadable).Count -eq 0 -and @($gaps.absent | Where-Object { $_.delay }).Count -eq 0) 'imports: every image was read, no delay import is absent'

# ---- 1. the BD-389 shape gives the reason ---------------------------------------------------------------------
$reading = Get-CompositorEventReading -Events $f.events -BootUtc $boot1 -DeviceName $f.device_name -InstanceId $f.instance_id
Check ($reading.restarts -eq 8 -and $reading.last_restart_count -eq 8) "reading 1: 8 compositor exits on this device (restart count $($reading.last_restart_count))"
Check ((@($reading.exit_codes) -join ',') -eq '0x889800b2') "reading 1: exit code $(@($reading.exit_codes) -join ',')"
Check ($reading.faults -eq 8 -and (@($reading.fault_modules) -join ',') -eq 'dwmcore.dll') "reading 1: 8 dwm.exe faults in $(@($reading.fault_modules) -join ',')"
Check ($reading.session_stop -eq 1 -and $reading.removed -eq 1) "reading 1: DxgKrnl 465 x$($reading.session_stop), Kernel-PnP 1011 of this device x$($reading.removed) (not the two ACPI devices)"
Check ($reading.other_restarts -eq 0 -and $reading.earlier -eq 0) "reading 1: no exit on another device and no event of these kinds before this boot ($($reading.earlier))"
$cause = Get-CompositorStartCause -Reading $reading -Imports $gaps -Record $rec1 -BootUtc $boot1 -ProblemCode 43
$first = [string]@($cause.reasons)[0]
Check (@($cause.reasons).Count -eq 8) "cause 1: one compositor reason and 7 missing imports ($(@($cause.reasons).Count) reasons)"
Check ($first -match 'dwm\.exe' -and $first -match '\b8 times\b' -and $first -match '0x889800b2' -and $first -match 'problem code 43') "cause 1: $first"
Check (@($cause.reasons) -contains 'desktop/bc250d3d_router.dll imports VCRUNTIME140.dll, which this system does not have') 'cause 1: the router names its runtime import'
Check ([string]$cause.advice -match 'Visual C\+\+ Redistributable' -and [string]$cause.advice -match 'x64') "cause 1: advice '$($cause.advice)'"
Check (@($cause.lines).Count -ge 12) "cause 1: $(@($cause.lines).Count) evidence lines for the diagnostics file"
$d = Get-StartFailureDecision -ProblemCode 43 -DevicePresent $true -DeviceStatus 'Error' -Record $rec1 -BootUtc $boot1 `
    -HealthFlags $null -LastStage 79 -Compositor $cause
Check ($d.collect -and @($d.reasons) -contains $first -and $d.advice -eq $cause.advice) 'decision 1: the cause is a reason of the decision and its advice is the decision''s'
$text = @(Get-DiagnosticsDecisionLines -Decision $d)
Check (@($text | Where-Object { $_ -eq "reason: $first" }).Count -eq 1 -and @($text | Where-Object { $_ -like 'advice: *' }).Count -eq 1 -and @($text | Where-Object { $_ -like 'compositor: *' }).Count -ge 12) "decision 1: the file block has the reason, the advice and the evidence ($($text.Count) lines)"
$line = Get-StartConfirmCauseLine -Cause $cause -ExitCode 5
Check ($line -like "cause: $first*" -and $line -like '*Visual C++ Redistributable*') "start-confirm 1: $line"
Check ($null -eq (Get-StartConfirmCauseLine -Cause $cause -ExitCode 0)) 'start-confirm 1: a confirmed run (exit 0) writes no cause line'

# ---- 2. restarts with another primary display device give no reason ----------------------------------------------
$elsewhere = @($f.events | ForEach-Object {
    $m = $_.Message.Replace('Primary display device ID: BC-250 GPU (amdgpu-wddm))', 'Primary display device ID: Microsoft Basic Display Adapter)')
    [pscustomobject]@{ TimeCreated = $_.TimeCreated; LogName = $_.LogName; ProviderName = $_.ProviderName; Id = $_.Id; Level = $_.Level; Message = $m } })
$r2 = Get-CompositorEventReading -Events $elsewhere -BootUtc $boot1 -DeviceName $f.device_name -InstanceId $f.instance_id
Check ($r2.restarts -eq 0 -and $r2.other_restarts -eq 8 -and (@($r2.other_devices) -join ',') -eq 'Microsoft Basic Display Adapter') "reading 2: 8 exits on '$(@($r2.other_devices) -join ',')', none on this device"
$c2 = Get-CompositorStartCause -Reading $r2 -Imports $gaps -Record $rec1 -BootUtc $boot1 -ProblemCode 43
Check (@($c2.reasons).Count -eq 0) "cause 2: no reason ($(@($c2.reasons).Count))"
Check (@($c2.notes | Where-Object { $_ -match 'Microsoft Basic Display Adapter' }).Count -eq 1 -and @($c2.notes | Where-Object { $_ -like '*imports VCRUNTIME140.dll, which this system does not have' }).Count -eq 3) 'cause 2: the other device and the missing imports are notes'

# ---- 3. the events of an earlier boot give no reason -------------------------------------------------------------
# The second boot of the reading (13:08:12Z) with its own start record; the problem code stays 43 so that only
# the boot decides.
$r3 = Get-CompositorEventReading -Events $f.events -BootUtc $boot2 -DeviceName $f.device_name -InstanceId $f.instance_id
Check ($r3.restarts -eq 0 -and $r3.faults -eq 0 -and $r3.session_stop -eq 0 -and $r3.removed -eq 0 -and $r3.earlier -eq 18) "reading 3: nothing of this boot; $($r3.earlier) events of the earlier boot left out"
$c3 = Get-CompositorStartCause -Reading $r3 -Imports $gaps -Record $rec2 -BootUtc $boot2 -ProblemCode 43
Check (@($c3.reasons).Count -eq 0) "cause 3: no reason ($(@($c3.reasons).Count))"
$r3b = Get-CompositorEventReading -Events $f.events -BootUtc $null -DeviceName $f.device_name -InstanceId $f.instance_id
$c3b = Get-CompositorStartCause -Reading $r3b -Imports $gaps -Record $rec1 -BootUtc $null -ProblemCode 43
Check ($r3b.restarts -eq 0 -and @($c3b.reasons).Count -eq 0 -and @($c3b.notes | Where-Object { $_ -match 'boot time' }).Count -eq 1) 'cause 3: an unknown boot time places no event in this boot and gives no reason'
$c3c = Get-CompositorStartCause -Reading $reading -Imports $gaps -Record $rec1 -BootUtc $boot1 -ProblemCode 0
Check (@($c3c.reasons).Count -eq 0) 'cause 3: the same events on a working device (problem code 0) give no reason'

# ---- 4. missing imports without DWM events give a note only ------------------------------------------------------
$quiet = @($f.events | Where-Object { $_.ProviderName -ne 'Dwminit' -and $_.ProviderName -ne 'Application Error' -and $_.Id -ne 465 -and $_.Id -ne 1011 })
$r4 = Get-CompositorEventReading -Events $quiet -BootUtc $boot1 -DeviceName $f.device_name -InstanceId $f.instance_id
$c4 = Get-CompositorStartCause -Reading $r4 -Imports $gaps -Record $rec1 -BootUtc $boot1 -ProblemCode 43
Check (@($c4.reasons).Count -eq 0 -and @($c4.notes | Where-Object { $_ -like '* which this system does not have' }).Count -eq 7) "cause 4: 7 notes, no reason"
Check ([string]$c4.advice -match 'Visual C\+\+ Redistributable') 'cause 4: the advice still names the runtime'
$good = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' -Record $rec1 -BootUtc $boot1 `
    -HealthFlags 15 -HealthReadyMs 90000 -LastStage 50 -Compositor $c4
Check (-not $good.collect -and @($good.notes | Where-Object { $_ -like '*bc250d3d_router.dll imports VCRUNTIME140.dll*' }).Count -eq 1) 'decision 4: a started desktop with missing imports collects nothing and keeps the notes'
Check ($null -ne (Get-StartConfirmCauseLine -Cause $c4 -ExitCode 1) -and (Get-StartConfirmCauseLine -Cause $c4 -ExitCode 1) -like 'cause: none found*') 'start-confirm 4: an unconfirmed run says that it found no cause'
$none = Get-StartFailureDecision -ProblemCode 0 -DevicePresent $true -DeviceStatus 'OK' -Record $rec1 -BootUtc $boot1 `
    -HealthFlags 15 -HealthReadyMs 90000 -LastStage 50
Check (-not $none.collect -and $null -eq $none.advice) 'decision: without a cause the decision is as before'

# ---- the PE import reader ----------------------------------------------------------------------------------------
$sys = Join-Path $env:windir 'System32\kernel32.dll'
$k = Read-PeImportFile -Path $sys
Check ($k.machine -eq 'x64' -and @($k.import) -contains 'ntdll.dll' -and @($k.import).Count -gt 20) "reader: System32\kernel32.dll is x64 and imports $(@($k.import).Count) DLLs, ntdll.dll among them"
$wow = Join-Path $env:windir 'SysWOW64\kernel32.dll'
if (Test-Path -LiteralPath $wow) {
    $k32 = Read-PeImportFile -Path $wow
    Check ($k32.machine -eq 'x86' -and @($k32.import) -contains 'ntdll.dll') 'reader: SysWOW64\kernel32.dll is x86 and imports ntdll.dll'
}
$shell = Read-PeImportFile -Path (Join-Path $env:windir 'System32\shell32.dll')
Check (@($shell.delay).Count -gt 0) "reader: System32\shell32.dll has $(@($shell.delay).Count) delay imports"
function Test-Refused([string]$Name, [byte[]]$Bytes) {
    $threw = $null
    try { [void](ConvertFrom-PeImportBytes -Bytes $Bytes) } catch { $threw = $_.Exception.Message }
    Check ($null -ne $threw) "reader refuses $Name$(if ($threw) { ": $threw" })"
}
$kb = [IO.File]::ReadAllBytes($sys)
Test-Refused 'a text file' ([Text.Encoding]::ASCII.GetBytes('not a PE image at all, only text'))
$noPe = [byte[]]$kb.Clone(); $noPe[0x3c] = 0xF0; $noPe[0x3d] = 0xFF; $noPe[0x3e] = 0xFF; $noPe[0x3f] = 0x7F
Test-Refused 'an e_lfanew outside the image' $noPe
$cut = New-Object byte[] 4096; [Array]::Copy($kb, $cut, 4096)
Test-Refused 'kernel32.dll cut to 4 KiB (the import directory is outside the image)' $cut
$magic = [byte[]]$kb.Clone(); $pe = [BitConverter]::ToInt32($kb, 0x3c); $magic[$pe + 24] = 0x07; $magic[$pe + 25] = 0x01
Test-Refused 'an optional header that is neither PE32 nor PE32+' $magic
$away = [byte[]]$kb.Clone(); [Array]::Copy([BitConverter]::GetBytes([uint32]0x7FFFFF00), 0, $away, $pe + 24 + 112 + 8, 4)
Test-Refused 'an import directory in no section' $away
$broken = Find-AbsentImports -Images @([ordered]@{ path = 'desktop/broken.dll'; machine = $null; import = @(); delay = @(); error = 'a read outside the image' }) -Present $present
$cb = Get-CompositorStartCause -Reading $reading -Imports $broken -Record $rec1 -BootUtc $boot1 -ProblemCode 43
Check (@($broken.unreadable).Count -eq 1 -and @($cb.notes | Where-Object { $_ -like 'the imports of desktop/broken.dll cannot be read*' }).Count -eq 1) 'reader: an image it cannot judge is a note of its own, never "no missing import"'

# The driver set itself, when the workspace holds it: the PowerShell reader gives the names of pe_image.py, and the
# files stay unloaded.
$tree = $(if ($env:BC250_ROOT) { Join-Path $env:BC250_ROOT 'scratch\glm\efi-signing\cks-manual\run6-staged\files\payload\kmd' } else { '' })
if ($tree -and (Test-Path -LiteralPath $tree)) {
    $same = 0
    foreach ($i in $f.images) {
        $r = Read-PeImportFile -Path (Join-Path $tree $i.path)
        if ($r.machine -eq $i.machine -and (@($r.import) -join ',') -eq (@($i.import) -join ',') -and (@($r.delay) -join ',') -eq (@($i.delay) -join ',')) { $same++ }
        else { "  differs: $($i.path)" }
    }
    Check ($same -eq $f.images.Count) "reader: the attempt 6 driver set reads as pe_image.py reads it ($same of $($f.images.Count))"
    $loaded = @([Diagnostics.Process]::GetCurrentProcess().Modules | Where-Object { $_.ModuleName -match '^(bc250d3d|amdgpu_wddm)' })
    Check ($loaded.Count -eq 0) "reader: no file of the driver set is loaded into this process ($($loaded.Count))"
} else { '  SKIP the attempt 6 driver set: BC250_ROOT does not hold it' }

# ---- the task's one reading of the GPU device ---------------------------------------------------------------------
# start-confirm.ps1 reads the device for the fallback's view and for the cause line of an unconfirmed run. Both
# go through one function, so that the hardware id is in one place of that script.
$tokens = $null; $parseErrors = $null
$task = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Installer 'start-confirm.ps1'), [ref]$tokens, [ref]$parseErrors)
$ids = @($task.FindAll({ param($a) $a -is [Management.Automation.Language.StringConstantExpressionAst] -or $a -is [Management.Automation.Language.ExpandableStringExpressionAst] }, $true) |
    Where-Object { $_.Value -match 'DEV_13FE' })
$cims = @($task.FindAll({ param($a) $a -is [Management.Automation.Language.CommandAst] -and $a.GetCommandName() -eq 'Get-CimInstance' -and $a.Extent.Text -match 'Win32_PnPEntity' }, $true))
$callers = @($task.FindAll({ param($a) $a -is [Management.Automation.Language.CommandAst] -and $a.GetCommandName() -eq 'Get-StartConfirmGpuDevices' }, $true) |
    ForEach-Object { $f = $_.Parent; while ($f -and $f -isnot [Management.Automation.Language.FunctionDefinitionAst]) { $f = $f.Parent }; $(if ($f) { $f.Name } else { 'top level' }) })
Check (@($parseErrors).Count -eq 0 -and $ids.Count -eq 1 -and $cims.Count -eq 1) "start-confirm.ps1: the hardware id $($ids.Count) time(s), one Win32_PnPEntity query ($($cims.Count))"
Check ((($callers | Sort-Object) -join ',') -eq 'Get-FallbackView,top level') "start-confirm.ps1: the fallback view and the cause line read the device through Get-StartConfirmGpuDevices ($($callers -join ','))"

if ($fail) { "FAIL: $fail check(s)"; exit 1 }
'PASS: all checks'
exit 0
