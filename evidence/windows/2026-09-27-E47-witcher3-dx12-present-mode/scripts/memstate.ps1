# Read-only memory/system state snapshot (Codex 118): physical, commit, pools, game/DWM process memory, boot and KMD health.
# Usage: memstate.ps1 -Tag <name>; writes C:\BC250\m12\witcher3-dx12\memstate-<tag>.json and prints it.
param([string]$Tag = 'snapshot')
$ErrorActionPreference = 'Continue'
$os = Get-CimInstance Win32_OperatingSystem
$ctr = Get-Counter -Counter '\Memory\Available Bytes', '\Memory\Committed Bytes', '\Memory\Commit Limit', '\Memory\Pool Paged Bytes', '\Memory\Pool Nonpaged Bytes', '\Memory\Cache Bytes', '\Memory\Free & Zero Page List Bytes', '\Memory\Standby Cache Normal Priority Bytes', '\Memory\Modified Page List Bytes' -ErrorAction SilentlyContinue
$counters = @{}
if ($ctr) { foreach ($s in $ctr.CounterSamples) { $counters[($s.Path -replace '^\\\\[^\\]+', '')] = [long]$s.CookedValue } }
$procs = @{}
foreach ($n in 'witcher3', 'dwm', 'csrss', 'bc250mon', 'python', 'powershell') {
  $list = Get-Process $n -ErrorAction SilentlyContinue
  if ($list) { $procs[$n] = @($list | ForEach-Object { @{ pid = $_.Id; ws_mb = [int]($_.WorkingSet64 / 1MB); private_mb = [int]($_.PrivateMemorySize64 / 1MB); paged_mb = [int]($_.PagedMemorySize64 / 1MB); nonpaged_kb = [int]($_.NonpagedSystemMemorySize64 / 1KB); handles = $_.HandleCount; threads = $_.Threads.Count } }) }
}
$top = Get-Process | Sort-Object WorkingSet64 -Descending | Select-Object -First 8 | ForEach-Object { @{ name = $_.ProcessName; pid = $_.Id; ws_mb = [int]($_.WorkingSet64 / 1MB); private_mb = [int]($_.PrivateMemorySize64 / 1MB) } }
$health = (& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' health read 2>&1 | Out-String).Trim()
$clock = (& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' clock read 2>&1 | Out-String).Trim()
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
$tctl = if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { [double]$Matches[1] } else { -1 }
$kmd = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.DeviceName -like '*BC250*' -or $_.DeviceName -like '*BC-250*' -or $_.InfName -like '*bc250*' } | Select-Object -First 1
$snap = [ordered]@{
  tag = $Tag
  utc = [DateTime]::UtcNow.ToString('o')
  boot = $os.LastBootUpTime.ToString('o')
  total_visible_mb = [int]($os.TotalVisibleMemorySize / 1KB)
  free_physical_mb = [int]($os.FreePhysicalMemory / 1KB)
  total_virtual_mb = [int]($os.TotalVirtualMemorySize / 1KB)
  free_virtual_mb = [int]($os.FreeVirtualMemory / 1KB)
  counters_bytes = $counters
  processes = $procs
  top_working_set = @($top)
  kmd_health = $health
  kmd_clock = $clock
  tctl = $tctl
  kmd_driver = if ($kmd) { @{ version = $kmd.DriverVersion; inf = $kmd.InfName; device = $kmd.DeviceName } } else { $null }
  dwm_pid = ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
}
$json = $snap | ConvertTo-Json -Depth 5
$json | Set-Content -LiteralPath "C:\BC250\m12\witcher3-dx12\memstate-$Tag.json" -Encoding ASCII
$json
