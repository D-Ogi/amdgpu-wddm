# LAB K137 discrimination bundle (~2.5 min): fixed-work CPU timing (wall clock, not the counter), the perf counter
# beside it, GPU memory bandwidth (vkmembw), DPM line. Fresh state, then one GPU device restart, then the same again.
# Kills H5 (counter artifact) if the fixed work slows like the counter; tests H4 (memory path) with vkmembw.
param([switch]$NoRestart, [string]$Label = 'b1')
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
Add-Type -TypeDefinition @'
using System; using System.Diagnostics;
public static class K137Work {
    // A dependent integer chain: no memory traffic, one core. Returns iterations per microsecond.
    public static double Run(long n) {
        ulong x = 0x9E3779B97F4A7C15UL; var sw = Stopwatch.StartNew();
        for (long i = 0; i < n; i++) { x = x * 6364136223846793005UL + 1442695040888963407UL; x ^= x >> 29; }
        sw.Stop(); if (x == 42) Console.WriteLine("x");
        return n / (sw.Elapsed.TotalMilliseconds * 1000.0);
    }
}
'@
function Invoke-K137Bundle([string]$state) {
    $proc = [System.Diagnostics.Process]::GetCurrentProcess()
    $saved = $proc.ProcessorAffinity
    $proc.ProcessorAffinity = [IntPtr]4
    [System.Threading.Thread]::BeginThreadAffinity()
    [void][K137Work]::Run(50000000)
    $job = Start-Job { ((Get-Counter '\Processor Information(0,2)\% Processor Performance' -SampleInterval 1 -MaxSamples 4).CounterSamples | Measure-Object CookedValue -Average).Average }
    $rates = 1..3 | ForEach-Object { [K137Work]::Run(600000000) }
    $c = Receive-Job $job -Wait -AutoRemoveJob
    [System.Threading.Thread]::EndThreadAffinity()
    $proc.ProcessorAffinity = $saved
    $perf = [double]$c
    "$state $(Get-Date -Format HH:mm:ss) fixed work $(($rates | ForEach-Object { '{0:N1}' -f $_ }) -join ' / ') it/us, counter perf $([math]::Round($perf,1)) %"
    & cmd.exe /c "`"C:\BC250\vkmembw\vkmembw.exe`" > C:\BC250\tmp\k137-membw-$state.txt 2>&1"
    Get-Content "C:\BC250\tmp\k137-membw-$state.txt" | Where-Object { $_ -match 'GB/s|copy|read|write' -and $_ -notmatch '^\s+(type|heap) ' } | Select-Object -First 8 | ForEach-Object { '  ' + $_.Trim() }
    & $cli dpm 2>&1 | Select-String 'smu clocks|power' | Select-Object -First 2 | ForEach-Object { '  ' + $_.Line.Trim() }
}
Invoke-K137Bundle "$Label-A"
if (-not $NoRestart) {
    $d = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }
    & pnputil.exe /restart-device "$($d.InstanceId)" 2>&1 | Select-Object -Last 1
    Start-Sleep 30
    "device $((Get-PnpDevice -InstanceId $d.InstanceId).Status)"
    Invoke-K137Bundle "$Label-B"
}
