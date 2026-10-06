# Host black box: one line every 10 s with kernel pool, memory and handle figures of the development PC, so that
# a hang like the one of 2026-09-21 07:26 leaves a trace. Appends to a file under the workspace scratch directory,
# one open+close per line, so that the record survives a hard reset of this PC. Never on drive C:.
# BC250_ROOT is the workspace root; by default the parent directory of this repository.
param(
    [string]$Log = (Join-Path (& { if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path } }) 'scratch\hostwatch\hostwatch.log'),
    [int]$Every = 10
)
New-Item -ItemType Directory -Force (Split-Path -Parent $Log) | Out-Null
while ($true) {
    try {
        $m = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
        $p = Get-Process
        $top = $p | Sort-Object HandleCount -Descending | Select-Object -First 1
        $kd = @($p | Where-Object ProcessName -eq 'kd')
        $line = '{0} npp={1:N0}MB pp={2:N0}MB avail={3:N0}MB commit={4:N0}MB handles={5} top={6}:{7} kd={8}:{9}' -f `
            (Get-Date).ToString('s'), ($m.PoolNonpagedBytes / 1MB), ($m.PoolPagedBytes / 1MB), $m.AvailableMBytes,
            ($m.CommittedBytes / 1MB), ($p | Measure-Object HandleCount -Sum).Sum, $top.ProcessName, $top.HandleCount,
            $kd.Count, (($kd | Measure-Object HandleCount -Sum).Sum)
    } catch { $line = '{0} error {1}' -f (Get-Date).ToString('s'), $_.Exception.Message }
    Add-Content -Path $Log -Value $line -Encoding utf8
    Start-Sleep -Seconds $Every
}
