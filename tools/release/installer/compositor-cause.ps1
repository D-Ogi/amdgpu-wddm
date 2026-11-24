# BD-389: why the desktop did not start on a driver that started. The cause reader of the installer's verify step
# (install.ps1 Invoke-VerifyDiagnostics, then diagnostics.ps1 Get-StartFailureDecision) and of the start-confirm task
# (start-confirm.ps1, then start-confirm-core.ps1 Get-StartConfirmCauseLine).
#
# The case this file was written for: the rc3 driver set alone on a clean Windows 11 26200. The KMD start record said
# 'started', but three user-mode files of the desktop route imported the Visual C++ runtime, which that Windows does
# not have. dwm.exe loads those files, so it ended 8 times with 0x889800b2 in dwmcore.dll; Winlogon stopped the GPU
# use of the session (DxgKrnl 465), Kernel-PnP removed the device as failing (1011), and the device stood at problem
# code 43. The verify step reported problem code 43 and had no cause: it did not read the Application log and did not
# check the imports of the files.
#
# Two readings and one decision:
#   1. the events of THIS boot (Get-CompositorEventReading): the Dwminit exits that name this device as the primary
#      display device, the Application Error entries of dwm.exe, DxgKrnl-Admin 465 and Kernel-PnP 1011 of this
#      device. Events before the boot are counted apart and decide nothing. The texts are read by their parts that do
#      not depend on the language of Windows: the provider, the event id, the hex exit code, the device name and
#      the instance id. The restart count and the module name are read from the English text only.
#   2. the import tables of the DLLs in the driver's DriverStore folder (ConvertFrom-PeImportTable, the reader of
#      tools/quality/pe_image.py in Windows PowerShell 5.1): each imported DLL that is not an API set, not a
#      KnownDLL, not in the file's own folder and not in the system folder of the file's architecture is missing.
#      The reader reads bytes and never loads a DLL. It fails closed: an image that it cannot judge is a note of its
#      own and never "no missing import".
#   3. the decision (Get-CompositorStartCause): a start record of this boot that says 'started', problem code 43 and
#      at least one compositor exit on this device in this boot are a reason. The missing imports are reasons with
#      it, and notes without it. A cause never confirms a start and never changes an exit code.
#
# Every function above the I/O section is pure, and tools\release\test-compositor-cause.ps1 tests them with the real
# readings of the lab unit. Read-CompositorStartCause is the only function that reads the computer, and it never
# throws. Windows PowerShell 5.1 syntax only. Dot-source it after start-confirm-core.ps1, whose
# Test-StartRecordFromEarlierBoot it uses.

$script:CompositorMaxEvents = 64              # per query
$script:CompositorMaxImages = 64              # DLLs of the DriverStore folder
$script:CompositorMaxDescriptors = 4096       # per import table; a real table has some tens
$script:CompositorMaxNameBytes = 512          # a DLL or function name is shorter than MAX_PATH
$script:CompositorStoppedProblem = 43         # CM_PROB_FAILED_POST_START
# The DLL families of the Visual C++ runtime that Windows does not ship (tools/quality/runtime_import_gate.py has the
# whole rule and its sources): the advice names the redistributable for these.
$script:CompositorVcRuntime = '^(vcruntime\d|msvcp(\d{3}|[7-9]\d)|msvcr(\d{3}|[7-9]\d)|concrt\d|vccorlib\d|vcomp\d|vcamp\d)'
# The event queries of Read-CompositorStartCause. Each names its log, because a provider answers only in the logs it
# writes to.
$script:CompositorEventQueries = @(
    [ordered]@{ log = 'Application'; provider = 'Dwminit'; id = $null }
    [ordered]@{ log = 'Application'; provider = 'Application Error'; id = 1000 }
    [ordered]@{ log = 'Microsoft-Windows-DxgKrnl-Admin'; provider = $null; id = 465 }
    [ordered]@{ log = 'Microsoft-Windows-Kernel-PnP/Device Management'; provider = $null; id = 1011 }
)

# ---- the PE import reader ---------------------------------------------------------------------------------------
# The layout is the one of tools/quality/pe_image.py (Microsoft PE format, "The .idata Section" and "Delay-Load
# Import Tables"): the import directory is data directory 1, 20-byte descriptors with the name RVA at offset 12; the
# delay-import directory is data directory 13, 32-byte descriptors with the attributes at offset 0 and the name at
# offset 4. Each array ends with a descriptor whose table and name fields are zero. A delay descriptor without
# attribute bit 0 can hold a virtual address in place of an RVA: a name at or above the image base is taken as one.
# $ReadAt is a script block (offset, count) that returns exactly count bytes or throws; $Length is the file size.
# Returns machine ('x64', 'x86', 'arm64' or the hex value), import and delay (DLL names in table order).
function ConvertFrom-PeImportTable {
    param([Parameter(Mandatory)][scriptblock]$ReadAt, [Parameter(Mandatory)][long]$Length)
    $read = {
        param([long]$At, [int]$Count)
        if ($At -lt 0 -or $At + $Count -gt $Length) { throw ('a read outside the image (offset 0x{0:x})' -f $At) }
        return [byte[]](& $ReadAt $At $Count)
    }
    if ($Length -lt 0x40) { throw 'not a PE image (no MZ header)' }
    $h = & $read 0 0x40
    if ($h[0] -ne 0x4D -or $h[1] -ne 0x5A) { throw 'not a PE image (no MZ header)' }
    $pe = [long][BitConverter]::ToUInt32($h, 0x3c)
    if ($pe + 4 -gt $Length) { throw 'not a PE image (no PE signature)' }
    $sig = & $read $pe 4
    if ($sig[0] -ne 0x50 -or $sig[1] -ne 0x45 -or $sig[2] -ne 0 -or $sig[3] -ne 0) { throw 'not a PE image (no PE signature)' }
    $coff = & $read ($pe + 4) 20
    $machine = [BitConverter]::ToUInt16($coff, 0)
    $count = [BitConverter]::ToUInt16($coff, 2)
    $optSize = [BitConverter]::ToUInt16($coff, 16)
    $opt = $pe + 24
    $dirs = @{}
    $base = [uint64]0
    if ($optSize) {
        $o = & $read $opt $optSize
        if ($optSize -lt 2) { throw 'the optional header is too short' }
        $magic = [BitConverter]::ToUInt16($o, 0)
        if ($magic -ne 0x10b -and $magic -ne 0x20b) { throw ('optional header magic 0x{0:x} is neither PE32 nor PE32+' -f $magic) }
        $wide = $magic -eq 0x20b
        $baseAt = $(if ($wide) { 24 } else { 28 })
        $countAt = $(if ($wide) { 108 } else { 92 })
        $dirAt = $(if ($wide) { 112 } else { 96 })
        if ($countAt + 4 -le $optSize) {
            $base = $(if ($wide) { [BitConverter]::ToUInt64($o, $baseAt) } else { [uint64][BitConverter]::ToUInt32($o, $baseAt) })
            $available = [BitConverter]::ToUInt32($o, $countAt)
            foreach ($index in 1, 13) {
                if ($index -lt $available -and $dirAt + 8 * $index + 8 -le $optSize) { $dirs[$index] = [long][BitConverter]::ToUInt32($o, $dirAt + 8 * $index) }
            }
        }
    }
    $sections = New-Object Collections.ArrayList
    $table = $opt + $optSize
    for ($s = 0; $s -lt $count; $s++) {
        $b = & $read ($table + 40 * $s) 40
        $vsize = [long][BitConverter]::ToUInt32($b, 8); $raw = [long][BitConverter]::ToUInt32($b, 16)
        [void]$sections.Add(@([long][BitConverter]::ToUInt32($b, 12), [Math]::Max($vsize, $raw), [long][BitConverter]::ToUInt32($b, 20)))
    }
    $offset = {
        param([long]$Rva)
        foreach ($x in $sections) { if ($x[0] -le $Rva -and $Rva -lt $x[0] + $x[1]) { return $x[2] + $Rva - $x[0] } }
        throw ('RVA 0x{0:x} is in no section' -f $Rva)
    }
    $text = {
        param([long]$Rva)
        $at = & $offset $Rva
        if ($at -ge $Length) { throw ('a name outside the image (RVA 0x{0:x})' -f $Rva) }
        $n = [int][Math]::Min([long]$script:CompositorMaxNameBytes, $Length - $at)
        $b = & $read $at $n
        $end = [Array]::IndexOf($b, [byte]0)
        if ($end -lt 0) { throw ('a name without its terminating zero (RVA 0x{0:x})' -f $Rva) }
        return [Text.Encoding]::GetEncoding(28591).GetString($b, 0, $end)
    }
    $names = {
        param([int]$Index)
        $found = New-Object Collections.ArrayList
        $rva = $dirs[$Index]
        if (-not $rva) { return }
        if ($Index -eq 1) { $size = 20; $nameAt = 12; $key = @(0, 12, 16) } else { $size = 32; $nameAt = 4; $key = @(4, 8, 12, 16) }
        $at = & $offset $rva
        for ($i = 0; $i -lt $script:CompositorMaxDescriptors; $i++) {
            $d = & $read $at $size
            $zero = $true
            foreach ($k in $key) { if ([BitConverter]::ToUInt32($d, $k) -ne 0) { $zero = $false } }
            if ($zero) { return $found.ToArray() }
            $name = [uint64][BitConverter]::ToUInt32($d, $nameAt)
            if ($Index -eq 13 -and -not ([BitConverter]::ToUInt32($d, 0) -band 1) -and $base -and $name -ge $base) { $name = $name - $base }
            if ($name -eq 0) { throw ('a descriptor of directory {0} without a name (offset 0x{1:x})' -f $Index, $at) }
            [void]$found.Add((& $text ([long]$name)))
            $at += $size
        }
        throw "directory $Index has no terminating entry within $($script:CompositorMaxDescriptors) descriptors"
    }
    $arch = switch ($machine) { 0x8664 { 'x64' } 0x14c { 'x86' } 0xaa64 { 'arm64' } default { '0x{0:x}' -f $machine } }
    return [ordered]@{ machine = $arch; import = @(& $names 1); delay = @(& $names 13) }
}

# The reader over bytes in memory.
function ConvertFrom-PeImportBytes {
    param([Parameter(Mandatory)][byte[]]$Bytes)
    $data = $Bytes
    $at = { param([long]$At, [int]$Count) $b = New-Object byte[] $Count; [Array]::Copy($data, $At, $b, 0, $Count); return ,$b }.GetNewClosure()
    return ConvertFrom-PeImportTable -ReadAt $at -Length $data.Length
}

# ---- the missing imports ----------------------------------------------------------------------------------------
# $Images: one map per DLL, with path, machine, import and delay (ConvertFrom-PeImportTable) or error (why the
# reader could not judge it). $Present: a script block (DLL name, machine, folder of the image) that says whether
# the loader can find the DLL. API sets (api-ms-*, ext-ms-*) are the loader's own contracts and are not asked about.
# Returns absent (file, dll, machine, delay) and unreadable (file, why).
function Find-AbsentImports {
    param([AllowNull()][AllowEmptyCollection()]$Images, [Parameter(Mandatory)][scriptblock]$Present)
    $absent = New-Object Collections.ArrayList
    $unreadable = New-Object Collections.ArrayList
    foreach ($i in @($Images)) {
        if ($null -eq $i) { continue }
        if ($i.error) { [void]$unreadable.Add([ordered]@{ file = [string]$i.path; why = [string]$i.error }); continue }
        $folder = Split-Path -Parent ([string]$i.path)
        foreach ($kind in 'import', 'delay') {
            foreach ($dll in @($i[$kind])) {
                if (-not $dll -or $dll -match '^(api|ext)-ms-') { continue }
                if (-not (& $Present ([string]$dll) ([string]$i.machine) $folder)) {
                    [void]$absent.Add([ordered]@{ file = [string]$i.path; dll = [string]$dll; machine = [string]$i.machine; delay = ($kind -eq 'delay') })
                }
            }
        }
    }
    return [ordered]@{ absent = @($absent); unreadable = @($unreadable) }
}

# ---- the events of this boot ------------------------------------------------------------------------------------
# A UTC time from a reading: a local time is converted, a time without a kind is taken as UTC.
function ConvertTo-CompositorUtc {
    param([AllowNull()]$Time)
    if ($null -eq $Time) { return $null }
    $t = [datetime]$Time
    if ($t.Kind -eq [DateTimeKind]::Local) { return $t.ToUniversalTime() }
    return [datetime]::SpecifyKind($t, [DateTimeKind]::Utc)
}

# What one event says, or $null for an event of no interest here.
function Get-CompositorEventKind {
    param($Entry, [AllowNull()][string]$DeviceName, [AllowNull()][string]$InstanceId)
    $m = [string]$Entry.Message
    $provider = [string]$Entry.ProviderName
    $id = [int]$Entry.Id
    $log = [string]$Entry.LogName
    if ($provider -eq 'Dwminit' -and $m -match '0x[0-9A-Fa-f]{8}') {
        $code = $Matches[0].ToLowerInvariant()
        $count = $null
        if ($m -match 'Restart count: (\d+)') { $count = [int]$Matches[1] }
        $ours = $DeviceName -and $m.IndexOf($DeviceName, [StringComparison]::OrdinalIgnoreCase) -ge 0
        $device = $null
        if (-not $ours) { $device = $(if ($m -match 'Primary display device ID: (.*)\)\s*$') { $Matches[1].Trim() } else { 'a device that the text does not name in English' }) }
        return [ordered]@{ kind = $(if ($ours) { 'exit' } else { 'exit elsewhere' }); code = $code; count = $count; device = $device }
    }
    if ($provider -eq 'Application Error' -and $id -eq 1000 -and $m -match '\bdwm\.exe\b') {
        $module = $(if ($m -match 'Faulting module name: ([^,\s]+)') { $Matches[1] } else { $null })
        return [ordered]@{ kind = 'fault'; module = $module }
    }
    if ($log -eq 'Microsoft-Windows-DxgKrnl-Admin' -and $id -eq 465) { return [ordered]@{ kind = 'session stop' } }
    if ($log -eq 'Microsoft-Windows-Kernel-PnP/Device Management' -and $id -eq 1011 -and $InstanceId -and
        $m.IndexOf($InstanceId, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return [ordered]@{ kind = 'removed' } }
    return $null
}

# Pure: the compositor events of this boot. $Events are objects with TimeCreated, LogName, ProviderName, Id and
# Message (Get-WinEvent records, or the fixtures of the host test). An event before $BootUtc is counted in 'earlier'
# and in nothing else; with no boot time no event is placed in this boot.
function Get-CompositorEventReading {
    param([AllowNull()][AllowEmptyCollection()]$Events, [AllowNull()]$BootUtc, [AllowNull()][string]$DeviceName,
          [AllowNull()][string]$InstanceId)
    $boot = ConvertTo-CompositorUtc $BootUtc
    $r = [ordered]@{ boot_known = ($null -ne $boot); restarts = 0; last_restart_count = $null; exit_codes = @(); other_restarts = 0
        other_devices = @(); faults = 0; fault_modules = @(); session_stop = 0; removed = 0; earlier = 0; lines = @() }
    $codes = New-Object Collections.ArrayList; $others = New-Object Collections.ArrayList
    $modules = New-Object Collections.ArrayList; $lines = New-Object Collections.ArrayList
    foreach ($e in @(@($Events) | Where-Object { $null -ne $_ } | Sort-Object { (ConvertTo-CompositorUtc $_.TimeCreated) })) {
        $k = Get-CompositorEventKind -Entry $e -DeviceName $DeviceName -InstanceId $InstanceId
        if ($null -eq $k) { continue }
        $when = ConvertTo-CompositorUtc $e.TimeCreated
        if ($null -eq $boot) { continue }
        if ($when -lt $boot) { $r.earlier++; continue }
        switch ($k.kind) {
            'exit' {
                $r.restarts++
                if ($null -ne $k.count) { $r.last_restart_count = $k.count }
                if (-not $codes.Contains($k.code)) { [void]$codes.Add($k.code) }
            }
            'exit elsewhere' { $r.other_restarts++; if (-not $others.Contains($k.device)) { [void]$others.Add($k.device) } }
            'fault' { $r.faults++; if ($k.module -and -not $modules.Contains($k.module)) { [void]$modules.Add($k.module) } }
            'session stop' { $r.session_stop++ }
            'removed' { $r.removed++ }
        }
        $text = ([string]$e.Message -replace '\s+', ' ').Trim()
        if ($text.Length -gt 300) { $text = $text.Substring(0, 300) + ' ...' }
        [void]$lines.Add(('{0} {1} ({2} id {3}): {4}' -f $when.ToString('yyyy-MM-ddTHH:mm:ssZ'), $k.kind, $(if ($e.ProviderName) { [string]$e.ProviderName } else { [string]$e.LogName }), [int]$e.Id, $text))
    }
    $r.exit_codes = @($codes); $r.other_devices = @($others); $r.fault_modules = @($modules); $r.lines = @($lines)
    return $r
}

# ---- the decision -----------------------------------------------------------------------------------------------
# Pure. $Reading is Get-CompositorEventReading's, $Imports is Find-AbsentImports's, $Record is the KMD start record
# (Read-StartRecordFromKey), $ProblemCode is the device's problem code (a number or its CM_PROB_* name).
# Returns reasons, notes, advice (what the tester can do, or $null) and lines (the evidence for the diagnostics file).
function Get-CompositorStartCause {
    param([AllowNull()]$Reading, [AllowNull()]$Imports, [AllowNull()]$Record, [AllowNull()]$BootUtc, [AllowNull()]$ProblemCode)
    $reasons = New-Object Collections.ArrayList
    $notes = New-Object Collections.ArrayList
    $lines = New-Object Collections.ArrayList
    $absent = @(); $unreadable = @()
    if ($null -ne $Imports) { $absent = @($Imports.absent | Where-Object { $null -ne $_ }); $unreadable = @($Imports.unreadable | Where-Object { $null -ne $_ }) }
    $missing = @($absent | ForEach-Object { "$($_.file) $(if ($_.delay) { 'delay-imports' } else { 'imports' }) $($_.dll), which this system does not have" })
    $unread = @($unreadable | ForEach-Object { "the imports of $($_.file) cannot be read ($($_.why)), so this file is not judged" })
    if ($null -eq $BootUtc) { [void]$notes.Add('the boot time of this computer is unknown, so no compositor event can be placed in this boot') }
    $startedHere = ($null -ne $Record) -and ([string]$Record.Result -eq 'started') -and ($null -ne $BootUtc) -and
        -not (Test-StartRecordFromEarlierBoot -Record $Record -BootUtc $BootUtc)
    $p = [string]$ProblemCode
    $stopped = ($p -eq [string]$script:CompositorStoppedProblem) -or ($p -eq 'CM_PROB_FAILED_POST_START')
    $restarts = 0
    if ($null -ne $Reading) { $restarts = [int]$Reading.restarts }
    if ($restarts -gt 0) {
        $what = "the desktop compositor (dwm.exe) ended $restarts time$(if ($restarts -ne 1) { 's' }) on this device in this boot (exit code $(@($Reading.exit_codes) -join ', ')$(if ($null -ne $Reading.last_restart_count) { ", restart count $($Reading.last_restart_count)" }))"
        if ($startedHere -and $stopped) {
            $text = "the driver started, but $what, and Windows then stopped the device (problem code $($script:CompositorStoppedProblem))"
            $then = @()
            if ($Reading.session_stop) { $then += 'Windows ended the GPU use of the session (DxgKrnl 465)' }
            if ($Reading.removed) { $then += 'Windows removed the device as failing (Kernel-PnP 1011)' }
            if ($then.Count) { $text = "$text; $($then -join ' and ')" }
            [void]$reasons.Add($text)
            foreach ($m in $missing) { [void]$reasons.Add($m) }
        } else {
            $why = $(if (-not $startedHere) { "the start record of this boot does not say 'started'" } else { "the device reports no problem code $($script:CompositorStoppedProblem)" })
            [void]$notes.Add("$what; this is no reason here, because $why")
            foreach ($m in $missing) { [void]$notes.Add($m) }
        }
    } else { foreach ($m in $missing) { [void]$notes.Add($m) } }
    if ($null -ne $Reading -and $Reading.other_restarts) {
        [void]$notes.Add("the desktop compositor ended $($Reading.other_restarts) time(s) in this boot on another primary display device ($(@($Reading.other_devices) -join ', ')), which is not this driver's device")
    }
    foreach ($u in $unread) { [void]$notes.Add($u) }
    $advice = $null
    $runtime = @($absent | Where-Object { $_.dll -match $script:CompositorVcRuntime })
    if ($runtime.Count) {
        $arch = @($runtime | ForEach-Object { $_.machine } | Sort-Object -Unique)
        $advice = "install the Microsoft Visual C++ Redistributable for Visual Studio 2015-2022 ($($arch -join ' and ')) and restart the computer, or install a driver package whose files do not need it"
    } elseif ($absent.Count) { $advice = 'the files named above need DLLs that this system does not have: report it with the diagnostics file' }
    if ($null -ne $Reading) { foreach ($l in @($Reading.lines)) { [void]$lines.Add($l) } }
    foreach ($m in $missing) { [void]$lines.Add($m) }
    foreach ($u in $unread) { [void]$lines.Add($u) }
    return [ordered]@{ reasons = @($reasons); notes = @($notes); advice = $advice; lines = @($lines) }
}

# ---- I/O: the only function that reads the computer ------------------------------------------------------------
# One file's import tables, read through a read-only stream: the DLL is never loaded and never mapped as an image.
function Read-PeImportFile {
    param([Parameter(Mandatory)][string]$Path)
    $stream = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $s = $stream
        $at = {
            param([long]$At, [int]$Count)
            $b = New-Object byte[] $Count
            [void]$s.Seek($At, [IO.SeekOrigin]::Begin)
            $got = 0
            while ($got -lt $Count) { $n = $s.Read($b, $got, $Count - $got); if ($n -le 0) { throw ('a read outside the image (offset 0x{0:x})' -f $At) }; $got += $n }
            return ,$b
        }.GetNewClosure()
        return ConvertFrom-PeImportTable -ReadAt $at -Length $stream.Length
    } finally { $stream.Dispose() }
}

# The KnownDLLs of this Windows (Session Manager\KnownDLLs): the loader maps these from its own section objects.
function Get-CompositorKnownDlls {
    $names = New-Object Collections.ArrayList
    $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs')
    if ($null -eq $k) { return @() }
    try { foreach ($v in $k.GetValueNames()) { $d = [string]$k.GetValue($v); if ($d -like '*.dll') { [void]$names.Add($d.ToLowerInvariant()) } } }
    finally { $k.Dispose() }
    return @($names)
}

# The folder of the driver's DriverStore copy: the folder of the bc250kmd service image.
function Get-CompositorDriverDirectory {
    param([string]$Service = 'bc250kmd')
    $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SYSTEM\CurrentControlSet\Services\$Service")
    if ($null -eq $k) { throw "the $Service service is not registered" }
    try { $image = [string]$k.GetValue('ImagePath', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) } finally { $k.Dispose() }
    if (-not $image) { throw "the $Service service has no ImagePath" }
    $p = $image -replace '^\\\?\?\\', ''
    $p = $p -replace '^\\SystemRoot\\', ($env:windir + '\') -replace '^%SystemRoot%\\', ($env:windir + '\')
    if ($p -match '^(?i)System32\\') { $p = Join-Path $env:windir $p }
    return (Split-Path -Parent $p)
}

# The whole cause for this boot. Never throws: a part that cannot be read is a note of the cause. $EventReader
# (query, limit) and $DriverDirectory replace the host readings in a test.
function Read-CompositorStartCause {
    param([AllowNull()]$BootUtc, [AllowNull()]$Record, [AllowNull()]$ProblemCode, [AllowNull()][string]$InstanceId,
          [AllowNull()][string]$DeviceName, [string]$DriverDirectory = '', [scriptblock]$EventReader)
    $failures = New-Object Collections.ArrayList
    $events = New-Object Collections.ArrayList
    if (-not $EventReader) { $EventReader = { param($query, $limit) Get-WinEvent -FilterHashtable $query -MaxEvents $limit -ErrorAction Stop } }
    if ($null -ne $BootUtc) {
        foreach ($q in $script:CompositorEventQueries) {
            # Windows PowerShell 5.1 reads StartTime as a local time.
            $filter = @{ LogName = $q.log; StartTime = ([datetime](ConvertTo-CompositorUtc $BootUtc)).ToLocalTime() }
            if ($q.provider) { $filter['ProviderName'] = $q.provider }
            if ($q.id) { $filter['Id'] = $q.id }
            try {
                foreach ($e in @(& $EventReader $filter $script:CompositorMaxEvents)) {
                    if ($null -eq $e) { continue }
                    [void]$events.Add([pscustomobject]@{ TimeCreated = (ConvertTo-CompositorUtc $e.TimeCreated); LogName = $q.log
                        ProviderName = [string]$e.ProviderName; Id = [int]$e.Id; Message = [string]$e.Message })
                }
            } catch {
                if ([string]$_.FullyQualifiedErrorId -notlike 'NoMatchingEventsFound*' -and [string]$_.Exception.Message -notmatch 'No events were found') {
                    [void]$failures.Add("the $($q.log) log$(if ($q.provider) { " ($($q.provider))" }) cannot be read: $($_.Exception.Message)")
                }
            }
        }
    }
    $reading = Get-CompositorEventReading -Events $events -BootUtc $BootUtc -DeviceName $DeviceName -InstanceId $InstanceId
    $images = New-Object Collections.ArrayList
    $present = { param($Dll, $Machine, $Directory) $true }
    try {
        if (-not $DriverDirectory) { $DriverDirectory = Get-CompositorDriverDirectory }
        $files = @(Get-ChildItem -LiteralPath $DriverDirectory -Recurse -File -Filter '*.dll' -ErrorAction Stop | Sort-Object FullName)
        if ($files.Count -gt $script:CompositorMaxImages) {
            [void]$failures.Add("the driver folder holds $($files.Count) DLLs; the first $($script:CompositorMaxImages) were checked")
            $files = @($files | Select-Object -First $script:CompositorMaxImages)
        }
        foreach ($f in $files) {
            $rel = $f.FullName.Substring($DriverDirectory.TrimEnd('\').Length + 1)
            try { $i = Read-PeImportFile -Path $f.FullName; [void]$images.Add([ordered]@{ path = $rel; full = $f.FullName; machine = $i.machine; import = $i.import; delay = $i.delay; error = $null }) }
            catch { [void]$images.Add([ordered]@{ path = $rel; full = $f.FullName; machine = $null; import = @(); delay = @(); error = [string]$_.Exception.Message }) }
        }
        $known = @(Get-CompositorKnownDlls)
        $native = $(if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) { Join-Path $env:windir 'Sysnative' } else { Join-Path $env:windir 'System32' })
        $wow = $(if ([Environment]::Is64BitOperatingSystem) { Join-Path $env:windir 'SysWOW64' } else { Join-Path $env:windir 'System32' })
        $root = $DriverDirectory
        $present = {
            param($Dll, $Machine, $Directory)
            $name = ([string]$Dll).ToLowerInvariant()
            if ($known -contains $name) { return $true }
            if ([IO.File]::Exists([IO.Path]::Combine($root, [string]$Directory, [string]$Dll))) { return $true }
            $system = $(if ($Machine -eq 'x86') { $wow } else { $native })
            return [IO.File]::Exists([IO.Path]::Combine($system, [string]$Dll))
        }.GetNewClosure()
    } catch { [void]$failures.Add("the driver's files cannot be listed: $($_.Exception.Message)") }
    $gaps = Find-AbsentImports -Images $images -Present $present
    $cause = Get-CompositorStartCause -Reading $reading -Imports $gaps -Record $Record -BootUtc $BootUtc -ProblemCode $ProblemCode
    foreach ($x in $failures) { $cause.notes = @($cause.notes) + @($x); $cause.lines = @($cause.lines) + @($x) }
    $cause.lines = @("read: $($events.Count) compositor event(s) of the four kinds since the boot, the imports of $($images.Count) DLL(s) in $(if ($DriverDirectory) { $DriverDirectory } else { 'no driver folder' })") + @($cause.lines)
    return $cause
}
