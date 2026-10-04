# The engine's contract with the setup window (docs/gui/interfaces-setup.md): events, the terminal result, cancel at
# safe points, one mutating engine at a time, the boot identity and the record of the newest install action (the
# state.json fields that void an older running-release witness, docs/gui/interfaces.md section 2). Dot-sourced after
# common.ps1, together with release-witness.ps1 (the boot identity), by install.ps1 and prepare-offline.ps1; the
# start-confirm task dot-sources only release-witness.ps1. Windows PowerShell 5.1 syntax only.
#
# The command-line installer behaves as before: without -EventsFile and -ResultFile nothing here writes a file, and
# without -Gui nothing here changes how the installer asks. With -Gui the engine never prompts, never elevates itself
# and never restarts Windows: what it needs (a consent, administrator rights, a restart) ends the run with a terminal
# result that names it, and the window asks the user.

$script:EngineContract = 'amdgpu-wddm.engine/1'
$script:EventSchema    = 'amdgpu-wddm.engine-event/1'
$script:ResultSchema   = 'amdgpu-wddm.engine-result/1'
$script:GuiMode        = $false
$script:EngineInvocationId   = $null
$script:EngineEventsFile     = $null
$script:EngineResultFile     = $null
$script:EngineCancelFile     = $null
$script:EventSeq       = 0
$script:EngineMode     = 'run'
$script:EngineStarted  = [DateTime]::UtcNow.ToString('o')
$script:EngineAction   = $null
$script:EnginePackageVersion = $null
$script:EnginePhaseBefore = $null
$script:EngineRestart  = $null
$script:EngineConsents = @()
$script:EngineFailedChecks = @()
$script:EngineLock     = $null
$script:EngineMutationSeen = $false
$script:EngineDeadline = $null          # [DateTime] UTC from -DeadlineUtc; $null: no deadline
$script:EngineDeadlineText = $null
$script:EngineStop     = $null          # the result's 'stop' record when a stop point ended the run
$script:EngineJob      = 'none'         # 'kill-on-close' once the engine runs in its own job object
$script:OnStop         = $null          # clean-up before a stop point ends the run (prepare-offline: its .partial folder)

function Initialize-Engine {
    param([bool]$Gui, [string]$InvocationId, [string]$EventsFile, [string]$ResultFile, [string]$Mode = 'run', [string]$DeadlineUtc)
    $script:GuiMode = $Gui
    # Every process the engine starts from here on belongs to its job and ends with it (section 10 of the contract).
    if ($Gui) { Enter-EngineJob }
    if ($DeadlineUtc) {
        $d = [DateTime]::MinValue
        if ([DateTime]::TryParse($DeadlineUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]'AdjustToUniversal, AssumeUniversal', [ref]$d)) {
            $script:EngineDeadline = $d; $script:EngineDeadlineText = $d.ToString('o')
        } else {
            # An unreadable deadline counts as passed: the run stops at its first stop point, before any change.
            $script:EngineDeadline = [DateTime]::MinValue; $script:EngineDeadlineText = "unreadable: $DeadlineUtc"
        }
    }
    if (-not $InvocationId -and ($Gui -or $EventsFile -or $ResultFile)) { $InvocationId = [guid]::NewGuid().ToString() }
    $script:EngineInvocationId = $InvocationId
    $script:EngineEventsFile = $(if ($EventsFile) { [IO.Path]::GetFullPath($EventsFile) } else { $null })
    $script:EngineResultFile = $(if ($ResultFile) { [IO.Path]::GetFullPath($ResultFile) } else { $null })
    # The window asks for a cancel by creating this file; the engine looks for it only at a safe point.
    $script:EngineCancelFile = $(if ($script:EngineEventsFile) { $script:EngineEventsFile + '.cancel' } else { $null })
    $script:EngineMode = $Mode
    # A result file left by another invocation never stands for this one.
    if ($script:EngineResultFile -and (Test-Path -LiteralPath $script:EngineResultFile)) { Remove-Item -LiteralPath $script:EngineResultFile -Force -ErrorAction SilentlyContinue }
    # The first system change of this run is recorded in state.json before it is made: a running-release witness written
    # earlier in this boot no longer names what runs. Every later change moves the time on (saved with the state).
    $script:OnFirstChange = { Set-MutationRecord -Save }
    $script:OnMutation = { Set-MutationRecord }
}

function ConvertTo-EngineJson($Value) { return ($Value | ConvertTo-Json -Depth 10 -Compress) }

# One event: one JSON object on one line, appended. The reader takes complete lines only.
function Write-EngineEvent {
    param([Parameter(Mandatory)][string]$Type, [Collections.IDictionary]$Data = @{})
    if (-not $script:EngineEventsFile) { return }
    $script:EventSeq++
    $e = [ordered]@{ schema = $script:EventSchema; invocation = $script:EngineInvocationId; seq = $script:EventSeq; utc = [DateTime]::UtcNow.ToString('o'); type = $Type }
    foreach ($k in $Data.Keys) { $e[[string]$k] = $Data[$k] }
    $line = (ConvertTo-EngineJson $e) + "`n"
    try {
        [void][IO.Directory]::CreateDirectory((Split-Path $script:EngineEventsFile))
        $fs = New-Object IO.FileStream -ArgumentList $script:EngineEventsFile, ([IO.FileMode]::Append), ([IO.FileAccess]::Write), ([IO.FileShare]::ReadWrite)
        try { $b = (New-Object Text.UTF8Encoding $false).GetBytes($line); $fs.Write($b, 0, $b.Length); $fs.Flush() } finally { $fs.Dispose() }
    } catch { Write-Log "   event not written: $($_.Exception.Message)" }
}

# Writes a file through a temporary file and a rename, so that a reader sees the old file or the whole new one.
function Write-FileAtomic([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory((Split-Path $Path))
    $tmp = $Path + '.tmp-' + [guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText($tmp, $Text, (New-Object Text.UTF8Encoding $false))
    if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) } else { [IO.File]::Move($tmp, $Path) }
}

# The boot (boot_id, boot_utc) comes from Get-BootIdentity in release-witness.ps1, the one place that defines it.

# ---- the record of the newest install action ----------------------------------------------------------------------
# state.json mutation_boot_id and mutation_utc (docs/gui/interfaces.md section 2): the boot and the start time of the
# newest step of a real run that changes Windows, the driver, files or settings. A dry run, a plan, verify and
# prepare-offline are not install actions. The first change of a run saves the state at once, before the change; later
# changes move the time on in memory and reach state.json with the next save of the state (every phase saves it). A
# running-release witness of the same boot that is older than mutation_utc no longer names what runs.
function Set-MutationRecord {
    param([switch]$Save)
    if ($script:DryRunMode -or -not $script:state) { return }
    $boot = Get-BootIdentity
    $first = -not $script:EngineMutationSeen
    Set-StateValue $script:state 'mutation_boot_id' $boot.boot_id
    Set-StateValue $script:state 'mutation_utc' ([DateTime]::UtcNow.ToString('o'))
    if ($Save) {
        # The record is the boundary that voids an older witness: no record, no change. The throw reaches the step's
        # trap before Invoke-Change runs the action, so the run ends as failed with nothing changed.
        try { Save-InstallState $script:state } catch { throw "the install action could not be recorded in $($script:StatePath) before the first change, so nothing was changed: $($_.Exception.Message)" }
    }
    $script:EngineMutationSeen = $true
    if ($first) { Write-EngineEvent 'install-action' ([ordered]@{ action = $script:EngineAction; boot_id = $boot.boot_id }) }
}

# ---- one mutating engine ----------------------------------------------------------------------------------------
# An exclusive handle on <state dir>\engine.lock for the life of the process; Windows closes it when the process ends,
# also after a crash. Dry runs and plans change nothing and take no lock.
function Enter-EngineLock([string]$Directory = $script:StateDir) {
    if ($script:DryRunMode) { return $true }
    try {
        [void][IO.Directory]::CreateDirectory($Directory)
        $script:EngineLock = New-Object IO.FileStream -ArgumentList (Join-Path $Directory 'engine.lock'), ([IO.FileMode]::OpenOrCreate), ([IO.FileAccess]::ReadWrite), ([IO.FileShare]::None)
        return $true
    } catch { return $false }
}

# ---- cancel and deadline (docs/gui/interfaces-setup.md section 10) -----------------------------------------------
# A run stops only at a stop point: before the first change of a phase, never in the middle of one, and, in a run that
# changes nothing (plan, dry run, verify, the copy of prepare-offline), at every stage boundary. At a stop point the
# engine ends the run when the caller created <EventsFile>.cancel or the -DeadlineUtc time has passed. Between stop
# points the engine finishes what it started; a deadline that passes there is announced once per stage ('deadline'
# event) and acted on at the next stop point. The window learns from 'cancel' events whether its Cancel button can work.
function Set-CancelAvailable([bool]$Available, [string]$Where) {
    Write-EngineEvent 'cancel' ([ordered]@{ available = $Available; where = $Where })
}
function Test-CancelRequested {
    return [bool]($script:EngineCancelFile -and (Test-Path -LiteralPath $script:EngineCancelFile))
}
function Test-DeadlinePassed {
    return [bool]($null -ne $script:EngineDeadline -and [DateTime]::UtcNow -ge $script:EngineDeadline)
}
function Invoke-CancelPoint([string]$Where) {
    $by = $(if (Test-CancelRequested) { 'cancel' } elseif (Test-DeadlinePassed) { 'deadline' } else { $null })
    if (-not $by) { return }
    if ($script:OnStop) { try { & $script:OnStop } catch { Write-Log "   clean-up at the stop point failed: $($_.Exception.Message)" } }
    $script:EngineStop = [ordered]@{ by = $by; where = $Where; deadline_utc = $script:EngineDeadlineText }
    if ($by -eq 'cancel') {
        Write-Host "Stopped at a safe point ($Where): cancelled from the setup window." -ForegroundColor Yellow
        Exit-Engine -Code 8 -Outcome 'cancelled' -MessageId $(if ($script:Mutated) { 'result.cancelled-after-changes' } else { 'result.cancelled' }) -Detail "cancel requested; stopped at the safe point '$Where'"
    }
    Write-Host "Stopped at a safe point ($Where): the deadline $($script:EngineDeadlineText) has passed." -ForegroundColor Yellow
    Exit-Engine -Code 8 -Outcome 'cancelled' -MessageId $(if ($script:Mutated) { 'result.deadline-after-changes' } else { 'result.deadline' }) -Detail "deadline $($script:EngineDeadlineText) passed; stopped at the safe point '$Where'"
}
# Outside a stop point: the deadline has passed, the engine finishes the stage it is in.
function Write-DeadlineNotice([string]$Where) {
    if (-not (Test-DeadlinePassed)) { return }
    Write-Log "   deadline $($script:EngineDeadlineText) passed in '$Where': this part cannot stop safely, it is finished first"
    Write-EngineEvent 'deadline' ([ordered]@{ passed = $true; where = $Where; deadline_utc = $script:EngineDeadlineText; action = 'finishing' })
}

# ---- the engine's child processes ---------------------------------------------------------------------------------
# A setup-window run (-Gui) puts itself into a new job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE before it starts
# any process. Every process it starts later (pnputil, bcdedit, the probes, their own children) is in that job and
# cannot break away. The only handle to the job is the engine's own, not inheritable: when the engine process ends in
# any way (its exit, a crash, or a caller that terminates it), Windows closes the handle and ends every process still
# in the job. At a normal end, Exit-Engine ends any left-over child itself and records it in the result ('children').
# Work that a Windows service performs for the engine (the PnP service's driver installation, WMI providers) is not a
# child of the engine and is not in the job.
if (-not ('AmdgpuWddmEngine.Job' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AmdgpuWddmEngine {
public static class Job {
    [StructLayout(LayoutKind.Sequential)] struct BasicLimit {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)] struct ExtendedLimit {
        public BasicLimit Basic; public IoCounters Io; public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimit info, int size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info, int size, out int returned);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    static IntPtr handle = IntPtr.Zero;
    // null when this process runs in its new kill-on-close job, else the reason.
    public static string Enter() {
        if (handle != IntPtr.Zero) return null;
        IntPtr job = CreateJobObjectW(IntPtr.Zero, null);
        if (job == IntPtr.Zero) return "CreateJobObject error " + Marshal.GetLastWin32Error();
        var info = new ExtendedLimit();
        info.Basic.LimitFlags = 0x2000;     // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if (!SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimit)))) return "SetInformationJobObject error " + Marshal.GetLastWin32Error();
        if (!AssignProcessToJobObject(job, GetCurrentProcess())) return "AssignProcessToJobObject error " + Marshal.GetLastWin32Error();
        handle = job;
        return null;
    }
    // The ids of the processes in the job now (this process included); empty without a job.
    public static long[] Processes() {
        if (handle == IntPtr.Zero) return new long[0];
        int n = 1024, size = 8 + n * IntPtr.Size, got;
        IntPtr buf = Marshal.AllocHGlobal(size);
        try {
            if (!QueryInformationJobObject(handle, 3, buf, size, out got)) return new long[0];   // JobObjectBasicProcessIdList
            int count = Marshal.ReadInt32(buf, 4);
            var r = new long[count];
            for (int i = 0; i < count; i++) r[i] = Marshal.ReadIntPtr(buf, 8 + i * IntPtr.Size).ToInt64();
            return r;
        } finally { Marshal.FreeHGlobal(buf); }
    }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();
    // Ends one process only while it is still in this job and is not this process: a process id that was reused by
    // another program after the child ended (DWM, a service) is never touched. True when the process was told to end.
    public static bool EndMember(long pid) {
        if (handle == IntPtr.Zero || pid == GetCurrentProcessId()) return false;
        IntPtr p = OpenProcess(0x1001, false, (uint)pid);   // PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION
        if (p == IntPtr.Zero) return false;
        try {
            bool member;
            if (!IsProcessInJob(p, handle, out member) || !member) return false;
            return TerminateProcess(p, 1);
        } finally { CloseHandle(p); }
    }
}
}
'@
}
function Enter-EngineJob {
    $why = [AmdgpuWddmEngine.Job]::Enter()
    if ($why) { $script:EngineJob = "none: $why"; Write-Log "   no job object for the engine's children: $why" } else { $script:EngineJob = 'kill-on-close' }
}
# At the end of a run: every process the engine started and that still runs is ended now (the job would end it with the
# engine anyway). Returns the result's 'children' record.
function Close-EngineChildren {
    if ($script:EngineJob -ne 'kill-on-close') { return [ordered]@{ job = $script:EngineJob; left_at_exit = $null; ended = $null } }
    $left = @([AmdgpuWddmEngine.Job]::Processes() | Where-Object { $_ -ne $PID })
    foreach ($p in $left) { [void][AmdgpuWddmEngine.Job]::EndMember($p) }
    # A process counts as ended when it is gone (one may end by itself, or with its parent, before its own turn).
    $running = $left
    for ($i = 0; $i -lt 50 -and $running.Count; $i++) {
        $running = @($left | Where-Object { Get-Process -Id ([int]$_) -ErrorAction SilentlyContinue })
        if ($running.Count) { Start-Sleep -Milliseconds 100 }
    }
    $ended = $left.Count - $running.Count
    if ($left.Count) { Write-Log "   ended $ended of $($left.Count) child process(es) still running at the end of the run: $($left -join ', ')" }
    return [ordered]@{ job = 'kill-on-close'; left_at_exit = $left.Count; ended = $ended }
}

# Host tests only (AMDGPU_WDDM_TEST_CHILD_SECONDS, honoured in a dry run or a plan): a cmd.exe that waits on ping, a
# child and a grandchild that outlive their step, so that the tests can show that both end with the engine.
function Start-EngineTestChild {
    $s = 0
    if (-not $script:DryRunMode -or -not [int]::TryParse([string]$env:AMDGPU_WDDM_TEST_CHILD_SECONDS, [ref]$s) -or $s -le 0) { return }
    $p = Start-Process -FilePath (Join-Path $env:windir 'System32\cmd.exe') -ArgumentList '/d', '/c', "ping -n $s 127.0.0.1 >nul" -WindowStyle Hidden -PassThru
    Write-EngineEvent 'test-child' ([ordered]@{ pid = $p.Id })
}

# ---- the terminal result ----------------------------------------------------------------------------------------
# Written once, at the end of every run that got this far, also after a failed step (the trap). The window believes
# the result only when its invocation is the one it started; the exit code alone is never the outcome.
function Exit-Engine {
    param([Parameter(Mandatory)][int]$Code, [Parameter(Mandatory)][string]$Outcome, [string]$MessageId, [string]$Detail, [string]$Step)
    $phaseAfter = $null
    try { if ($script:state -and $script:state.PSObject.Properties['phase']) { $phaseAfter = [string]$script:state.phase } } catch { }
    $restart = $script:EngineRestart
    if (-not $restart) { $restart = [ordered]@{ required = $false } }
    $children = Close-EngineChildren
    $r = [ordered]@{
        schema = $script:ResultSchema
        invocation = $script:EngineInvocationId
        engine = [ordered]@{ contract = $script:EngineContract; package_version = $script:EnginePackageVersion }
        mode = $script:EngineMode
        dry_run = [bool]$script:DryRunMode
        started_utc = $script:EngineStarted
        ended_utc = [DateTime]::UtcNow.ToString('o')
        boot = (Get-BootIdentity)
        action = $script:EngineAction
        outcome = $Outcome
        exit_code = $Code
        mutated = [bool]$script:Mutated
        nothing_changed = -not [bool]$script:Mutated
        phase_before = $script:EnginePhaseBefore
        phase_after = $phaseAfter
        restart = $restart
        consents_needed = @($script:EngineConsents)
        failed_checks = @($script:EngineFailedChecks)
        message_id = $MessageId
        step = $(if ($Step) { $Step } else { $null })
        detail = $Detail
        log = $script:LogPath
        deadline_utc = $script:EngineDeadlineText
        stop = $script:EngineStop
        children = $children
    }
    Write-EngineEvent 'result' ([ordered]@{ outcome = $Outcome; exit_code = $Code; message_id = $MessageId; mutated = [bool]$script:Mutated; stop = $(if ($script:EngineStop) { $script:EngineStop.by } else { $null }) })
    if ($script:EngineResultFile) {
        try { Write-FileAtomic $script:EngineResultFile ($r | ConvertTo-Json -Depth 10) } catch { Write-Log "   result not written: $($_.Exception.Message)" }
    }
    Write-Log "   engine result: $Outcome (exit $Code$(if ($MessageId) { ", $MessageId" }))"
    if ($script:EngineLock) { try { $script:EngineLock.Dispose() } catch { } }
    exit $Code
}
