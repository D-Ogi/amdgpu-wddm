# The poll of start-confirm.ps1 (the logon task "amdgpu-wddm start confirm"), on its real schedule and read-only, with
# the time of each step. measure-flash.ps1 runs it next to the start window, to see whether the window's flashes
# follow the poll. Windows PowerShell 5.1:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File poll-repro.ps1 -Out <csv> [-Seconds 45] [-Tools <dir>]
#
#   -Tools    the installed tools folder (default C:\Program Files\amdgpu-wddm\tools): bc250kmd_cli.exe,
#             bc250control.dll and start-confirm-core.ps1 come from there, as the task takes them
#   -Seconds  how long to poll
#   -Out      the CSV: poll,step,qpc0,qpc1,result. qpc0 and qpc1 are QueryPerformanceCounter ticks
#             ([Diagnostics.Stopwatch]::GetTimestamp), the clock of flash-watch.exe
#
# The same calls in the same order as Invoke-Poll of start-confirm.ps1: the start health READ through
# bc250control.dll (Bc250StartHealth op 0), then Get-FallbackView: Win32_PnPEntity of the GPU through CIM,
# bc250kmd_cli info, bc250kmd_cli stages. The next poll is due 5 s after the start of the last one, checked every
# 250 ms, as the task's loop does. The interop is compiled at the first READ, as in the task. Never a CONFIRM: this
# script changes nothing in the driver, and writes only -Out.
param([string]$Tools = 'C:\Program Files\amdgpu-wddm\tools', [int]$Seconds = 45, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $Tools 'start-confirm-core.ps1')
$cli = Join-Path $Tools 'bc250kmd_cli.exe'
$dll = [IO.Path]::GetFullPath((Join-Path $Tools 'bc250control.dll'))
$writer = New-Object IO.StreamWriter($Out, $false, (New-Object Text.UTF8Encoding($false)))
$writer.WriteLine('poll,step,qpc0,qpc1,result')
$writer.Flush()

function Write-Step([int]$Poll, [string]$Step, [long]$Qpc0, [string]$Result) {
    $qpc1 = [Diagnostics.Stopwatch]::GetTimestamp()
    $writer.WriteLine(('{0},{1},{2},{3},{4}' -f $Poll, $Step, $Qpc0, $qpc1, ($Result -replace '[,\r\n]', ' ')))
    $writer.Flush()
}

# As start-confirm.ps1: a native program's output as one text, and its exit code.
function Invoke-Cli([string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $text = (& $cli @Arguments 2>&1 | ForEach-Object { [string]$_ }) -join "`n" } finally { $ErrorActionPreference = $old }
    return @{ text = $text; code = $LASTEXITCODE }
}

function Invoke-ReproPoll([int]$Poll) {
    $t = [Diagnostics.Stopwatch]::GetTimestamp()
    $result = ''
    try {
        if (-not ('Bc250StartConfirm.Kmd' -as [type])) { Add-Type -TypeDefinition (Get-StartConfirmInteropSource -DllPath $dll) }
        $d = New-Object Bc250StartConfirm.Health
        $status = [Bc250StartConfirm.Kmd]::Bc250StartHealth(0, 0, 0, [ref]$d, 96)
        $result = 'status 0x{0:X8} flags {1}' -f $status, $d.Flags
    } catch { $result = "error $($_.Exception.Message)" }
    Write-Step $Poll 'health' $t $result

    $t = [Diagnostics.Stopwatch]::GetTimestamp()
    $dev = @(Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'PCI\\VEN_1002&DEV_13FE%'" -ErrorAction SilentlyContinue)
    Write-Step $Poll 'cim' $t "devices $($dev.Count)"

    $t = [Diagnostics.Stopwatch]::GetTimestamp()
    $info = Invoke-Cli @('info')
    Write-Step $Poll 'info' $t "exit $($info.code)"

    $t = [Diagnostics.Stopwatch]::GetTimestamp()
    $stages = Invoke-Cli @('stages')
    Write-Step $Poll 'stages' $t "exit $($stages.code)"
}

$clock = [Diagnostics.Stopwatch]::StartNew()
$nextPoll = 0.0
$poll = 0
try {
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        $now = $clock.Elapsed.TotalSeconds
        if ($now -ge $nextPoll) {
            $poll++
            $nextPoll = $now + 5.0
            Invoke-ReproPoll $poll
        }
        Start-Sleep -Milliseconds 250
    }
} finally { $writer.Dispose() }
exit 0
