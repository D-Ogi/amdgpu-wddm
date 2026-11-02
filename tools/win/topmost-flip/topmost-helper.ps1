# The console-session half of topmost-flip.ps1 (BD-187). Do not run it by hand: topmost-flip.ps1 copies it into its
# work folder and starts it through a scheduled task of the console user with the highest run level, because Task
# Manager runs elevated and a window of a lower integrity level cannot change it (UIPI).
#
# It starts Task Manager, then waits for command files from the session-0 script: cmd-<n>.txt with one word, on, off
# or quit. For on and off it sets Task Manager's own "Always on top" option through UI Automation (the Settings page,
# the check box -ToggleName), and checks WS_EX_TOPMOST of the window afterwards. When UI Automation does not find the
# check box, or the window style does not follow it, it sets the style itself with SetWindowPos and says so: method
# "window" is the mechanism, not the reported option. Each command gets result-<n>.json. quit turns the option off,
# closes the window with WM_CLOSE and ends. The helper also ends at -Seconds, the same way.
#
# Task Manager can replace its window when the option changes: in both lab runs of 2026-11-02 the window handle was
# invalid (Win32 error 1400) right after the toggle, and a Taskmgr process with a new id drew the next frames. So the
# helper never keeps a window handle across a command. Before each command and after each toggle it checks the handle
# with IsWindow and, when the window is gone, finds the TaskManagerWindow of whichever Taskmgr process runs now. Each
# change goes into the answer as "replaced" (old and new process id and window), and topmost and band are read from
# the window that is there after the toggle.
#
# A hung window answers neither UI Automation nor SetWindowPos in time: in run 1 of 2026-10-29 each FromHandle took
# about 20 s and the window stayed blank. So every command first sends WM_NULL with SendMessageTimeout (2 s, abort if
# hung). A window that exists and does not answer gets no UI Automation and no SetWindowPos, and its answer says
# hung=True, with the Win32 error and IsHungAppWindow. A window that no longer exists is not hung: the helper follows
# its replacement. UI Automation runs in its own runspace and is given up after -UiaSeconds. Each answer comes in
# about 12 s or less, and in 23 s at most (a window replaced before and after the toggle, and then hung).
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$SettingsName = 'Settings',
    [string]$ToggleName = 'Always on top',
    [int]$Seconds = 160,
    [int]$UiaSeconds = 6
)
$ErrorActionPreference = 'Continue'
$deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
$logPath = Join-Path $Dir 'helper.log'

function Write-HelperLog([string]$Text) {
    Add-Content -LiteralPath $logPath -Value ('{0} {1}' -f [DateTime]::UtcNow.ToString('o'), $Text) -Encoding ascii
}

function Write-Json([string]$Name, $Value) {
    $path = Join-Path $Dir $Name
    $temporary = $path + '.tmp'
    Set-Content -LiteralPath $temporary -Value ($Value | ConvertTo-Json -Compress -Depth 5) -Encoding ascii
    Move-Item -LiteralPath $temporary -Destination $path -Force
}

Add-Type -Namespace Bc250 -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr hwnd, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hwnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindow(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetWindowTextLength(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr hwnd, System.Text.StringBuilder name, int count);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetWindowLong(System.IntPtr hwnd, int index);
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
public static extern bool SetWindowPos(System.IntPtr hwnd, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l, uint flags, uint ms, out System.IntPtr result);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsHungAppWindow(System.IntPtr hwnd);
// Read only, and not documented: the z-order band of a window (1 is the desktop band). It says whether Task Manager's
// option moves the window to another band instead of setting WS_EX_TOPMOST. -1 when the export is absent.
[System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "GetWindowBand")]
static extern bool GetWindowBandRaw(System.IntPtr hwnd, out uint band);
public static long Band(System.IntPtr hwnd) {
    try { uint band; return GetWindowBandRaw(hwnd, out band) ? (long)band : -1; } catch (System.EntryPointNotFoundException) { return -1; }
}
public static string ClassOf(System.IntPtr hwnd) {
    var name = new System.Text.StringBuilder(256); GetClassName(hwnd, name, name.Capacity); return name.ToString();
}
public static uint OwnerOf(System.IntPtr hwnd) { uint pid; GetWindowThreadProcessId(hwnd, out pid); return pid; }
// The visible top-level window of class TaskManagerWindow that has a title and belongs to the process, or zero.
public static System.IntPtr TaskManagerWindow(uint pid) {
    System.IntPtr found = System.IntPtr.Zero;
    EnumWindows(delegate(System.IntPtr h, System.IntPtr l) {
        uint owner; GetWindowThreadProcessId(h, out owner);
        if (owner == pid && IsWindowVisible(h) && GetWindowTextLength(h) > 0 && ClassOf(h) == "TaskManagerWindow") { found = h; return false; }
        return true;
    }, System.IntPtr.Zero);
    return found;
}
'@

$WS_EX_TOPMOST = 0x8
function Get-Topmost([IntPtr]$Window) { return (([Bc250.Win]::GetWindowLong($Window, -20)) -band $WS_EX_TOPMOST) -ne 0 }
function Format-Window([IntPtr]$Window) { return ('0x{0:X}' -f [int64]$Window) }
# The window calls that Invoke-Topmost makes go through these functions only, so that host\check-helper.ps1 of the
# acceptance kit can run the follow logic with made-up windows.
function Get-Band([IntPtr]$Window) { return [Bc250.Win]::Band($Window) }
function Test-Current($Current) { return [Bc250.Win]::IsWindow($Current.window) -and [Bc250.Win]::OwnerOf($Current.window) -eq [uint32]$Current.pid }
function Test-Window([IntPtr]$Window) { return [Bc250.Win]::IsWindow($Window) }
function Set-WindowTopmost([IntPtr]$Window, [bool]$On) {
    # HWND_TOPMOST -1, HWND_NOTOPMOST -2; SWP_NOSIZE 1, SWP_NOMOVE 2, SWP_NOACTIVATE 0x10.
    $after = if ($On) { [IntPtr](-1) } else { [IntPtr](-2) }
    $set = [Bc250.Win]::SetWindowPos($Window, $after, 0, 0, 0, 0, 0x13)
    return [pscustomobject]@{ set = $set; error = [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
}

# The Task Manager window that the helper works on: { pid, window }, or $null when no Taskmgr process has one.
$script:current = $null
# The window changes seen in the command that runs now.
$script:events = @()

# The TaskManagerWindow of the newest Taskmgr process that has one. The script refuses to start when Task Manager runs,
# so every Taskmgr process is one that this helper started or that Task Manager started for itself.
function Find-TaskManager {
    $all = @(Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue)
    $sorted = @($all | Sort-Object -Property @{ Expression = { try { $_.StartTime } catch { [DateTime]::MinValue } } } -Descending)
    foreach ($p in $sorted) {
        $w = [Bc250.Win]::TaskManagerWindow([uint32]$p.Id)
        if ($w -ne [IntPtr]::Zero) { return [pscustomobject]@{ pid = [int]$p.Id; window = $w } }
    }
    return $null
}

# The current window when it still exists, else its replacement, found within $WaitMs. A change is logged and added to
# $script:events with $When ('before' the command or 'after-toggle').
function Resolve-Current([int]$WaitMs, [string]$When) {
    $c = $script:current
    if ($c -and (Test-Current $c)) { return $c }
    $found = $null
    $until = [DateTime]::UtcNow.AddMilliseconds($WaitMs)
    do {
        $found = Find-TaskManager
        if ($found) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    $change = [pscustomobject]@{ when = $When; utc = [DateTime]::UtcNow.ToString('o')
        from_pid = $(if ($c) { $c.pid } else { $null }); from_window = $(if ($c) { Format-Window $c.window } else { $null })
        to_pid = $(if ($found) { $found.pid } else { $null }); to_window = $(if ($found) { Format-Window $found.window } else { $null })
        taskmgr_pids = @(Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue | ForEach-Object { [int]$_.Id }) }
    $script:events += $change
    Write-HelperLog "window replaced ($When): pid $($change.from_pid) window $($change.from_window) -> pid $($change.to_pid) window $($change.to_window); Taskmgr pids $($change.taskmgr_pids -join ',')"
    $script:current = $found
    return $found
}

# WM_NULL through SendMessageTimeout with SMTO_ABORTIFHUNG. ok: the window's thread answered within $Ms. valid: the
# window still exists (error 1400, ERROR_INVALID_WINDOW_HANDLE, means that it does not). Only a valid window that does
# not answer is hung.
function Test-Responsive([IntPtr]$Window, [int]$Ms = 2000) {
    $reply = [IntPtr]::Zero
    $answer = [Bc250.Win]::SendMessageTimeout($Window, 0, [IntPtr]::Zero, [IntPtr]::Zero, 0x2, [uint32]$Ms, [ref]$reply)
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $ok = $answer -ne [IntPtr]::Zero
    $valid = [Bc250.Win]::IsWindow($Window)
    $hungApp = $valid -and [Bc250.Win]::IsHungAppWindow($Window)
    if (-not $ok) { Write-HelperLog "wm_null: no answer in $Ms ms, error $errorCode, IsWindow $valid, IsHungAppWindow $hungApp" }
    return [pscustomobject]@{ ok = $ok; error = $(if ($ok) { 0 } else { $errorCode }); valid = $valid; hung_app = $hungApp }
}

# Task Manager's own option, in a runspace of its own so that a provider that does not answer costs at most
# $UiaSeconds. The check box is on the Settings page, which is opened only when the box is not on the page shown.
# Returns the toggle state after the change, 'not-found:<what>' or 'timeout'. An empty state means that the check box
# went away with its window after the toggle.
$uiaScript = {
    param([long]$Handle, [bool]$On, [string]$SettingsName, [string]$ToggleName)
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $root = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$Handle)
    $scope = [Windows.Automation.TreeScope]::Descendants
    $byName = { param($name) New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $name) }
    $box = $root.FindFirst($scope, (& $byName $ToggleName))
    if (-not $box) {
        $nav = $root.FindFirst($scope, (& $byName $SettingsName))
        if (-not $nav) { return "not-found:$SettingsName" }
        $pattern = $null
        if ($nav.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) { $pattern.Select() }
        elseif ($nav.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) { $pattern.Invoke() }
        else { return "not-found:pattern of $SettingsName" }
        $until = [DateTime]::UtcNow.AddSeconds(3)
        do { Start-Sleep -Milliseconds 250; $box = $root.FindFirst($scope, (& $byName $ToggleName)) } while (-not $box -and [DateTime]::UtcNow -lt $until)
        if (-not $box) { return "not-found:$ToggleName" }
    }
    $toggle = $null
    if (-not $box.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$toggle)) { return "not-found:toggle pattern" }
    $want = if ($On) { [Windows.Automation.ToggleState]::On } else { [Windows.Automation.ToggleState]::Off }
    $changed = $false
    if ($toggle.Current.ToggleState -ne $want) { $toggle.Toggle(); $changed = $true }
    Start-Sleep -Milliseconds 400
    $state = ''
    try { $state = [string]$toggle.Current.ToggleState } catch { $state = '' }
    return "$state|$changed"
}

# Returns @{ state; changed }: state On, Off, '' (the box went away), timeout, no-result, error:<text>, not-found:<what>.
function Set-Option([IntPtr]$Window, [bool]$On) {
    $shell = [PowerShell]::Create()
    [void]$shell.AddScript($uiaScript).AddArgument([int64]$Window).AddArgument($On).AddArgument($SettingsName).AddArgument($ToggleName)
    $pending = $shell.BeginInvoke()
    if (-not $pending.AsyncWaitHandle.WaitOne($UiaSeconds * 1000)) {
        # Abandoned, not stopped: a Stop would wait for the same provider. The runspace ends with the helper.
        [void]$shell.BeginStop($null, $null)
        return [pscustomobject]@{ state = 'timeout'; changed = $null }
    }
    $text = 'no-result'
    try {
        $out = @($shell.EndInvoke($pending))
        foreach ($e in $shell.Streams.Error) { Write-HelperLog "uia: $e" }
        $shell.Dispose()
        if ($out.Count) { $text = [string]$out[-1] }
    } catch { $text = "error:$($_.Exception.InnerException.Message)" }
    if ($text -match '^(On|Off|Indeterminate|)\|(True|False)$') {
        return [pscustomobject]@{ state = $Matches[1]; changed = ($Matches[2] -eq 'True') }
    }
    return [pscustomobject]@{ state = $text; changed = $null }
}

# One answer. topmost, band, pid and window are of the current window at the time of the answer.
function New-Answer([string]$Method, [bool]$Hung, $Option, $Changed, $Set, $ErrorCode, $Before, $Probe) {
    $c = $script:current
    return [pscustomobject]@{ method = $Method; hung = $Hung; option = $Option; toggled = $Changed; setwindowpos = $Set
        win32_error = $ErrorCode
        topmost = $(if ($c) { Get-Topmost $c.window } else { $false }); band = $(if ($c) { Get-Band $c.window } else { -1 })
        before = $Before; pid = $(if ($c) { $c.pid } else { $null }); window = $(if ($c) { Format-Window $c.window } else { $null })
        gone = (-not $c); replaced = @($script:events)
        wm_null_error = $(if ($Probe -and -not $Probe.ok) { $Probe.error } else { $null })
        hung_app = $(if ($Probe) { $Probe.hung_app } else { $null }) }
}

function Invoke-Topmost([bool]$On) {
    $script:events = @()
    $c = Resolve-Current 2000 'before'
    if (-not $c) { return New-Answer 'none' $false $null $null $null $null $null $null }
    $before = [pscustomobject]@{ topmost = (Get-Topmost $c.window); band = (Get-Band $c.window); pid = $c.pid; window = (Format-Window $c.window) }
    $probe = Test-Responsive $c.window
    if (-not $probe.ok -and -not $probe.valid) {
        # The window went away between the check and WM_NULL: follow it once.
        $c = Resolve-Current 2000 'before'
        if (-not $c) { return New-Answer 'none' $false $null $null $null $null $before $probe }
        $probe = Test-Responsive $c.window
    }
    if (-not $probe.ok) { return New-Answer 'none' ([bool]$probe.valid) $null $null $null $null $before $probe }

    $old = $c
    $option = Set-Option $old.window $On
    # Task Manager can replace its window after the toggle. Wait up to 2 s until the old window is gone or shows the
    # requested style, then take the window that is there.
    $until = [DateTime]::UtcNow.AddSeconds(2)
    while ((Test-Window $old.window) -and (Get-Topmost $old.window) -ne $On -and [DateTime]::UtcNow -lt $until) {
        Start-Sleep -Milliseconds 200
    }
    $c = Resolve-Current 3000 'after-toggle'
    if (-not $c) { return New-Answer 'none' $false $option.state $option.changed $null $null $before $probe }
    $replaced = $c.window -ne $old.window
    if ($replaced) {
        # A new window gets 2 s for its first paint before its style counts.
        $probe = Test-Responsive $c.window 2000
        if (-not $probe.ok) { return New-Answer 'option' ([bool]$probe.valid) $option.state $option.changed $null $null $before $probe }
    }
    $method = 'option'
    $set = $null
    $errorCode = $null
    $optionKnown = $option.state -in @('On', 'Off') -or ($replaced -and $option.state -eq '')
    if (-not $optionKnown -or (Get-Topmost $c.window) -ne $On) {
        # The option did not take, or it does not use WS_EX_TOPMOST. Set the style itself, synchronously, and only
        # after another WM_NULL check, so that a window that hung meanwhile does not block this call.
        $method = 'window'
        $probe = Test-Responsive $c.window
        if ($probe.ok) {
            $pos = Set-WindowTopmost $c.window $On
            $set = $pos.set
            $errorCode = $pos.error
            Write-HelperLog "setwindowpos $(if ($On) { 'topmost' } else { 'notopmost' }) -> $set, error $errorCode, topmost $(Get-Topmost $c.window)"
        } else {
            return New-Answer 'none' ([bool]$probe.valid) $option.state $option.changed $null $null $before $probe
        }
    }
    return New-Answer $method $false $option.state $option.changed $set $errorCode $before $probe
}

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-HelperLog "start dir $Dir settings '$SettingsName' toggle '$ToggleName' seconds $Seconds uia $UiaSeconds s elevated $elevated"
if (Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue) {
    Write-Json 'ready.json' ([pscustomobject]@{ ok = $false; reason = 'Task Manager already runs' }); exit 2
}
[void](Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\Taskmgr.exe') -PassThru)
$until = [DateTime]::UtcNow.AddSeconds(20)
do {
    Start-Sleep -Milliseconds 250
    # Taskmgr.exe can hand over to a second instance; take the window of whichever Taskmgr runs.
    $script:current = Find-TaskManager
} while (-not $script:current -and [DateTime]::UtcNow -lt $until)
if (-not $script:current) {
    Write-Json 'ready.json' ([pscustomobject]@{ ok = $false; reason = 'no TaskManagerWindow in 20 s' }); exit 1
}
$window = $script:current.window
# A window can be slow at its first paint; it gets 10 s to answer WM_NULL before the trial calls it hung.
$responsive = $false
$until = [DateTime]::UtcNow.AddSeconds(10)
do { $responsive = (Test-Responsive $window 1000).ok; if (-not $responsive) { Start-Sleep -Milliseconds 500 } } while (-not $responsive -and [DateTime]::UtcNow -lt $until)
$windowClass = [Bc250.Win]::ClassOf($window)
Write-Json 'ready.json' ([pscustomobject]@{ ok = $true; pid = $script:current.pid; window = (Format-Window $window); class = $windowClass
    responsive = $responsive; topmost = (Get-Topmost $window); band = [Bc250.Win]::Band($window); elevated = $elevated })
Write-HelperLog "taskmgr pid $($script:current.pid) window $(Format-Window $window) class $windowClass responsive $responsive topmost $(Get-Topmost $window) band $([Bc250.Win]::Band($window))"

$n = 1
$quit = $false
while (-not $quit) {
    $command = Join-Path $Dir ('cmd-{0}.txt' -f $n)
    if ([DateTime]::UtcNow -ge $deadline) { $word = 'quit'; Write-HelperLog 'deadline: quit' }
    elseif (Test-Path -LiteralPath $command) { $word = (Get-Content -LiteralPath $command -Raw).Trim() }
    else { Start-Sleep -Milliseconds 200; continue }
    $began = [DateTime]::UtcNow
    $result = $null
    switch ($word) {
        'on' { $result = Invoke-Topmost $true }
        'off' { $result = Invoke-Topmost $false }
        'quit' {
            # The option is persistent in Task Manager: it goes back off before the window closes. Then every Taskmgr
            # window gets WM_CLOSE once, the current one first, and the helper waits up to 6 s for all of them to end.
            $result = Invoke-Topmost $false
            $posted = @{}
            $until = [DateTime]::UtcNow.AddSeconds(6)
            do {
                $left = @(Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue)
                if ($left.Count -eq 0) { break }
                foreach ($p in $left) {
                    $w = [Bc250.Win]::TaskManagerWindow([uint32]$p.Id)
                    $key = Format-Window $w
                    if ($w -ne [IntPtr]::Zero -and -not $posted.ContainsKey($key)) {
                        $posted[$key] = $p.Id
                        [void][Bc250.Win]::PostMessage($w, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
                        Write-HelperLog "quit: WM_CLOSE to pid $($p.Id) window $key"
                    }
                }
                Start-Sleep -Milliseconds 250
            } while ([DateTime]::UtcNow -lt $until)
            $left = @(Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue | ForEach-Object { [int]$_.Id })
            $result | Add-Member -NotePropertyName ended -NotePropertyValue ($left.Count -eq 0)
            $result | Add-Member -NotePropertyName left -NotePropertyValue $left
            $quit = $true
        }
        default { $result = [pscustomobject]@{ method = 'none'; error = "unknown command '$word'" } }
    }
    $result | Add-Member -NotePropertyName command -NotePropertyValue $word
    $result | Add-Member -NotePropertyName ms -NotePropertyValue ([int]([DateTime]::UtcNow - $began).TotalMilliseconds)
    $result | Add-Member -NotePropertyName utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
    Write-Json ('result-{0}.json' -f $n) $result
    Write-HelperLog "cmd $n $word -> $($result | ConvertTo-Json -Compress -Depth 5)"
    $n++
}
exit 0
