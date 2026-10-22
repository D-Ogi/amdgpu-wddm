# C85 kill test, second form, bounded at 170 s. The lab overlay bc250mon repaints its panel every few seconds and also
# confirms a start by itself, so it is suspended for the test and resumed in finally. Then: restart the GPU device (a
# new start-health generation), wait until the start is a minute old AND no new picture came for 16 s, and ask the
# KMD to CONFIRM. The old rule refuses that reading (LastCompletionAge > 15 s); C85 says QUIET accepts it. Every line
# also goes to C:\BC250\tmp\c85\kill2.log.
param([int]$Bound = 170)
$log = 'C:\BC250\tmp\c85\kill2.log'
function Out-Line([string]$s) { $s; Add-Content -LiteralPath $log -Value $s }
Add-Type -Namespace C85 -Name Nt -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
'@
$cli = Get-ChildItem -Path 'C:\Program Files\amdgpu-wddm' -Recurse -Filter bc250kmd_cli.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
function Read-Health {
    $line = (& $cli health read 2>&1 | Select-Object -First 1) -join ''
    $h = @{ line = $line }
    foreach ($k in 'flags', 'generation', 'epoch', 'completed', 'age_ms', 'ready_ms') {
        if ($line -match "\b$k=(\d+)") { $h[$k] = [uint64]$Matches[1] }
    }
    $h
}
$t0 = [DateTime]::UtcNow
function Stamp { '+{0,6:N1}s' -f ([DateTime]::UtcNow - $t0).TotalSeconds }
Out-Line "utc $($t0.ToString('o')) boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
$mon = @(Get-Process bc250mon -ErrorAction SilentlyContinue)
$suspended = @()
try {
    foreach ($p in $mon) {
        $r = [C85.Nt]::NtSuspendProcess($p.Handle)
        Out-Line "$(Stamp) suspend bc250mon $($p.Id): 0x$('{0:X8}' -f $r)"
        if ($r -eq 0) { $suspended += $p }
    }
    Out-Line "$(Stamp) before: $((Read-Health).line)"
    $dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
    $out = & pnputil.exe /restart-device "$($dev.InstanceId)" 2>&1
    Out-Line "$(Stamp) pnputil restart exit $LASTEXITCODE :: $(($out | Select-Object -Last 1) -join ' ')"
    $done = $false; $last = $null
    while (([DateTime]::UtcNow - $t0).TotalSeconds -lt $Bound) {
        Start-Sleep -Seconds 2
        $h = Read-Health
        if ($null -eq $h.flags) { Out-Line "$(Stamp) read: $($h.line)"; continue }
        if ($null -eq $last -or $h.completed -ne $last.completed -or $h.flags -ne $last.flags -or $h.generation -ne $last.generation) { Out-Line "$(Stamp) $($h.line)" }
        $last = $h
        if (($h.flags -band 7) -eq 7 -and $h.ready_ms -ge 61000 -and $h.age_ms -ge 16000) {
            Out-Line "$(Stamp) idle window: $($h.line)"
            if (($h.flags -band 8) -eq 8) { Out-Line "$(Stamp) NOT DECISIVE: this generation is already confirmed" }
            Out-Line "$(Stamp) confirm: $((& $cli health confirm $h.generation $h.epoch 2>&1) -join ' | ') exit $LASTEXITCODE"
            Out-Line "$(Stamp) after: $((Read-Health).line)"
            $done = $true; break
        }
    }
    if (-not $done) {
        $h = Read-Health
        Out-Line "$(Stamp) no idle window inside $Bound s: $($h.line)"
        Out-Line "$(Stamp) confirm (fresh or not): $((& $cli health confirm $h.generation $h.epoch 2>&1) -join ' | ') exit $LASTEXITCODE"
        Out-Line "$(Stamp) after: $((Read-Health).line)"
    }
} finally {
    foreach ($p in $suspended) {
        $r = [C85.Nt]::NtResumeProcess($p.Handle)
        Out-Line "$(Stamp) resume bc250mon $($p.Id): 0x$('{0:X8}' -f $r)"
    }
}
$d2 = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
Out-Line "$(Stamp) device $($d2.Status) $($d2.Problem); dwm $((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)"
