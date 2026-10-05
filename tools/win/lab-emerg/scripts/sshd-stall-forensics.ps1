# BD-051 forensics for a live sshd accept stall (run as SYSTEM over the emergency channel, before restart-sshd).
# 1. the listener's main-thread stack (send_rexec_state shape), 2. its early_child and w32 children tables read
# non-invasively from memory (RVAs of the public sshd.pdb of Win32-OpenSSH 9.8.3.0, sshd.exe B3962BFE only),
# 3. exit code and times of every pid in those tables, 4. every process holding a handle to one of the listener's
# W32PosixPipe pipes (handle64), 5. Security 4688/4689 events of those pids. Read-only; does not restart sshd.
param([int]$Minutes = 15)
$ErrorActionPreference = 'Continue'
$cdb = 'C:\BC250\tools\cdb\cdb.exe'
$h64 = 'C:\BC250\tools\handle64.exe'
$out = 'C:\BC250\tmp\sshd-stacks'
New-Item -ItemType Directory -Force $out | Out-Null
"utc $([DateTime]::UtcNow.ToString('o'))"
$p = Get-Process sshd -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no sshd.exe"; return }
$exe = 'C:\Program Files\OpenSSH\sshd.exe'
$hash = (Get-FileHash $exe).Hash.Substring(0, 8)
"sshd.exe pid $($p.Id) hash $hash"
Add-Type -Namespace B -Name K -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool i, uint pid);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetExitCodeProcess(IntPtr h, out uint code);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetProcessTimes(IntPtr h, out long c, out long e, out long k, out long u);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
'@
function ProcInfo([uint32]$procId) {
  $hp = [B.K]::OpenProcess(0x1000, $false, $procId)
  if ($hp -eq [IntPtr]::Zero) { return "pid ${procId}: cannot open (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
  $code = 0; $c = 0; $e = 0; $k = 0; $u = 0
  [void][B.K]::GetExitCodeProcess($hp, [ref]$code)
  [void][B.K]::GetProcessTimes($hp, [ref]$c, [ref]$e, [ref]$k, [ref]$u)
  [void][B.K]::CloseHandle($hp)
  $ct = [DateTime]::FromFileTimeUtc($c).ToString('HH:mm:ss.fff')
  $et = if ($code -eq 259) { 'running' } else { [DateTime]::FromFileTimeUtc($e).ToString('HH:mm:ss.fff') }
  $name = (Get-Process -Id $procId -ErrorAction SilentlyContinue).Name
  'pid {0} {1} created {2} exit {3} code 0x{4:X8} cpu_ms {5}' -f $procId, $name, $ct, $et, $code, [int](($k + $u) / 10000)
}
$log = Join-Path $out ("forensics-{0}.txt" -f [DateTime]::UtcNow.ToString('HHmmss'))
if ($hash -eq 'B3962BFE') {
  $base = '0x{0:x}' -f $p.MainModule.BaseAddress.ToInt64()
  $c = Join-Path $out 'forensics.cdb'
  @"
~*kn 12
dq $base+0xD8BA0 L10
dd $base+0xD8BA0+0x1000 L10
dd $base+0xD8BA0+0x1800 L2
dd $base+0xB7DC0 L1
r `$t0 = poi($base+0xB7DD0)
dd @`$t0 L60
qd
"@ | Set-Content -Encoding ascii $c
  & $cdb -pvr -p $p.Id -y 'C:\BC250\tools\cdb\nosym' -logo $log -cf $c 2>&1 | Out-Null
  $lines = @(Get-Content $log | Where-Object { $_ -notmatch '^ModLoad|^\*\*\*|^\s*$' } | ForEach-Object { [string]$_ })
  "== listener memory"
  $lines | Select-Object -Skip ([Math]::Max(0, ($lines | Select-String -SimpleMatch '~*kn' | Select-Object -First 1).LineNumber - 1))
  # pids from the w32 children table's process_id row (+0x1000), the three dump lines after its dd command
  $pids = @()
  $pidLines = $lines | Where-Object { $_ -match "dd $base\+0xD8BA0\+0x1000" }
  $idx = [Array]::IndexOf($lines, ($pidLines | Select-Object -First 1))
  if ($idx -ge 0) {
    foreach ($l in $lines[($idx + 1)..($idx + 3)]) {
      if ($l -match '^[0-9a-f`]{17}\s+(.*)$') { $pids += ($Matches[1] -split '\s+' | Where-Object { $_ -match '^[0-9a-f]{8}$' } | ForEach-Object { [Convert]::ToUInt32($_, 16) } | Where-Object { $_ -ne 0 }) }
    }
  }
  "== w32 children pids: $($pids -join ' ')"
  foreach ($x in ($pids | Select-Object -Unique)) { ProcInfo $x }
} else { "hash differs from the 9.8.3.0 build: memory tables skipped" }
"== listener pipe holders"
$tag = 'W32PosixPipe.{0:x8}' -f $p.Id
if (Test-Path $h64) {
  & $h64 -accepteula -nobanner -a $tag 2>&1 | ForEach-Object { [string]$_ } | Select-Object -First 60
} else { "handle64 missing at $h64" }
"== 4688/4689 of sshd-session in the last $Minutes min"
$since = (Get-Date).AddMinutes(-$Minutes)
Get-WinEvent -FilterHashtable @{ LogName = 'Security'; Id = 4688, 4689; StartTime = $since } -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'sshd-session|sshd\.exe' } | Sort-Object TimeCreated | ForEach-Object {
    $x = [xml]$_.ToXml(); $d = @{}; foreach ($n in $x.Event.EventData.Data) { $d[$n.Name] = $n.'#text' }
    if ($_.Id -eq 4688) {
      '{0} 4688 new {1} pid {2} parent {3}' -f $_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss.fff'), $d.NewProcessName, [Convert]::ToUInt32($d.NewProcessId, 16), [Convert]::ToUInt32($d.ProcessId, 16)
    } else {
      '{0} 4689 exit {1} pid {2} status {3}' -f $_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss.fff'), $d.ProcessName, [Convert]::ToUInt32($d.ProcessId, 16), $d.Status
    } }
"== tcp22"
Get-NetTCPConnection -LocalPort 22 -ErrorAction SilentlyContinue | Group-Object State | ForEach-Object { "$($_.Name) $($_.Count)" }
