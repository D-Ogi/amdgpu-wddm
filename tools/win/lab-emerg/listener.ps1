# Emergency channel to unit A, independent of sshd (owner request 2026-10-01 after two sshd accept stalls, BD-051).
# Runs as SYSTEM from the scheduled task "BC250 emergency channel" (at startup, restarted on failure).
# http.sys listener on TCP 8722, firewall: local subnet only. Protocol v2: POST /lab/ with headers X-Ts (unix s),
# X-Nonce (32 hex), X-Action, X-Arg (base64 of UTF-8) and X-Sig = HMAC-SHA256(key, ts\nnonce\naction\narg\nsha256(body))
# in hex; the body is raw data (a script for ps, file bytes for put, else empty), at most 16 MB. A timestamp more than
# 120 s off or a reused nonce is refused. Key C:\BC250\emergency\key.bin (ACL SYSTEM + Administrators).
# Only the actions below exist; nothing runs between requests (no polling, no spawning).
# ACLs (owner, 2026-10-01): overwriting an existing file keeps its security descriptor (sshd_config needs its own)
# and leaves <file>.emerg-<utc>.bak beside it; a new file inherits its directory's ACL and gets Administrators as owner,
# so the interactive user (an administrator) can read and change what SYSTEM wrote.
$ErrorActionPreference = 'Stop'
$base = 'C:\BC250\emergency'
$logFile = Join-Path $base 'listener.log'
$key = [IO.File]::ReadAllBytes((Join-Path $base 'key.bin'))
if ($key.Length -lt 32) { throw 'key too short' }
$hmac = [Security.Cryptography.HMACSHA256]::new($key)
$sha = [Security.Cryptography.SHA256]::Create()
$seen = @{}
$utf8 = New-Object Text.UTF8Encoding($false)
$maxBody = 16MB

function Log([string]$t) { try { Add-Content -LiteralPath $logFile -Value ('{0:o} {1}' -f [DateTime]::UtcNow, $t) -Encoding ascii } catch {} }
function Hex([byte[]]$b) { -join ($b | ForEach-Object { $_.ToString('x2') }) }
function Full([string]$p) { if (-not $p) { throw 'path missing' }; [IO.Path]::GetFullPath($p) }
function Acl([string]$p) {
    $lines = @(& icacls.exe $p 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "ACL read failed (icacls exit $LASTEXITCODE)" }
    @($lines | Select-Object -First 8 | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ })
}
function Owner-Admins([string]$p) {
    & icacls.exe $p /setowner '*S-1-5-32-544' 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Owner update failed (icacls exit $LASTEXITCODE); the file may already exist" }
}

function Status {
    $o = [ordered]@{}
    $o.now = [DateTime]::UtcNow.ToString('o')
    $o.boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    $s = Get-CimInstance Win32_Service -Filter "Name='sshd'"
    $o.sshd = '{0} pid {1}' -f $s.State, $s.ProcessId
    $p = Get-Process -Id $s.ProcessId -ErrorAction SilentlyContinue
    if ($p) { $o.sshd_start = $p.StartTime.ToUniversalTime().ToString('o') }
    $o.sshd_sessions = @(Get-Process -Name 'sshd-session' -ErrorAction SilentlyContinue).Count
    $o.port22_listen = @(Get-NetTCPConnection -LocalPort 22 -State Listen -ErrorAction SilentlyContinue).Count
    $o.port22_established = @(Get-NetTCPConnection -LocalPort 22 -State Established -ErrorAction SilentlyContinue).Count
    $o.processes = @(Get-Process -Name witcher3, dwm, steam, amdgpu_wddm_d3d12_queue, bc250kmd_cli, bc250rd_cli -ErrorAction SilentlyContinue |
        ForEach-Object { '{0} {1} {2:o}' -f $_.Name, $_.Id, $_.StartTime.ToUniversalTime() })
    $o.powershell = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | ForEach-Object {
        $c = [string]$_.CommandLine; if ($c.Length -gt 120) { $c = $c.Substring(0, 120) }; '{0} {1}' -f $_.ProcessId, $c })
    $o.openssh_last = @(Get-WinEvent -LogName 'OpenSSH/Operational' -MaxEvents 6 -ErrorAction SilentlyContinue | ForEach-Object {
        $m = ($_.Message -replace '\s+', ' '); if ($m.Length -gt 110) { $m = $m.Substring(0, 110) }
        '{0:HH:mm:ss} {1}' -f $_.TimeCreated.ToUniversalTime(), $m })
    $o
}

# The diagnostic USB stick, identified positively (CLAUDE.md: label, file system, bus, exact size; no other match).
function Usb-Stick {
    $v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
    if ($v.Count -ne 1) { throw "usb: $($v.Count) volumes labelled BC250DIAG" }
    $disk = Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk
    if ($v[0].FileSystem -ne 'FAT32' -or $disk.BusType -ne 'USB' -or $disk.Size -ne 64160400896) { throw 'usb: identity mismatch' }
    $root = "$($v[0].DriveLetter):\"
    [ordered]@{ root = $root; grub = Join-Path $root 'boot\grub\grub.cfg'; efi = Join-Path $root 'efi\boot\bootx64.efi'; off = Join-Path $root 'efi\boot\bootx64.off' }
}
function Usb-State($u) {
    $cfg = [IO.File]::ReadAllText($u.grub)
    [ordered]@{ root = $u.root; default = [regex]::Match($cfg, '(?m)^set default=(\S+)').Groups[1].Value
        loader = $(if (Test-Path $u.efi) { 'bootx64.efi (USB boots: Linux)' } elseif (Test-Path $u.off) { 'bootx64.off (falls through to NVMe: Windows)' } else { 'missing' })
        entries = @([regex]::Matches($cfg, '(?m)^\s*menuentry\s+[''"]([^''"]+)[''"]') | ForEach-Object { $_.Groups[1].Value }) }
}

function Run-Action([string]$action, [string]$arg, [byte[]]$body) {
    switch ($action) {
        'status' { return Status }
        'restart-sshd' { Restart-Service -Name sshd -Force; Start-Sleep -Seconds 2; return Status }
        'kill-game' {
            $g = @(Get-Process -Name witcher3 -ErrorAction SilentlyContinue)
            $g | Stop-Process -Force -ErrorAction SilentlyContinue
            return [ordered]@{ killed = $g.Count }
        }
        'desktop-cpu' {
            # The GPU DWM kill switch at the next boot (route.py release cpu --no-restart): DwmForceCpu=1 and a pending cpu
            # request (the RoutePending* values dwm-route.ps1 writes; route.py release verify checks it after the boot).
            # DWM is not restarted (BD-060: a forced DWM restart breaks WinUI pointer input until Windows restarts);
            # 'reboot reboot-now' applies it, 'kill-dwm accept-bd060' is the last resort for a hung compositor.
            $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
            $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\amdgpu-wddm\DesktopRouter', $true)
            if (-not $k) { throw 'router key missing' }
            try {
                $k.SetValue('DwmForceCpu', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
                foreach ($v in @(@('RoutePending', 'cpu'), @('RoutePendingBoot', $boot), @('RoutePendingUtc', [DateTime]::UtcNow.ToString('o')), @('RoutePendingBy', 'lab-emergency'))) {
                    $k.SetValue($v[0], $v[1], [Microsoft.Win32.RegistryValueKind]::String)
                }
                $k.Flush()
                $back = [ordered]@{ DwmForceCpu = $k.GetValue('DwmForceCpu'); RoutePending = $k.GetValue('RoutePending'); RoutePendingBoot = $k.GetValue('RoutePendingBoot') }
            } finally { $k.Dispose() }
            Log 'desktop-cpu: DwmForceCpu 1, cpu pending until Windows restarts'
            return [ordered]@{ written = $back; effect = 'cpu at the next restart of Windows (reboot reboot-now); DWM not restarted (BD-060)' }
        }
        'kill-dwm' {
            # LAST RESORT for a hung compositor (BD-060): stops DWM; winlogon starts a new one, but WinUI pointer input
            # stays broken until Windows restarts. Needs the arg accept-bd060.
            if ($arg -ne 'accept-bd060') { throw 'kill-dwm breaks WinUI pointer input until Windows restarts (BD-060); arg accept-bd060 required' }
            $before = @(Get-Process -Name dwm -ErrorAction SilentlyContinue | ForEach-Object { '{0}:{1:o}' -f $_.Id, $_.StartTime.ToUniversalTime() })
            Log ('kill-dwm (BD-060 accepted): ' + ($before -join ' '))
            Get-Process -Name dwm -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 5
            return [ordered]@{ stopped = $before; dwm = @(Get-Process -Name dwm -ErrorAction SilentlyContinue | ForEach-Object { '{0}:{1:o}' -f $_.Id, $_.StartTime.ToUniversalTime() })
                warning = 'WinUI pointer input stays broken until Windows restarts (BD-060)' }
        }
        'ps' {
            # Arbitrary PowerShell (the body, UTF-8) in a job, 120 s bound. arg: optional bound in seconds (<= 600).
            $limit = 120; if ($arg -match '^\d+$') { $limit = [Math]::Min(600, [int]$arg) }
            $sb = [scriptblock]::Create($utf8.GetString($body))
            $j = Start-Job -ScriptBlock $sb
            $done = Wait-Job $j -Timeout $limit
            $out = @(Receive-Job $j -ErrorAction Continue 2>&1 | ForEach-Object { [string]$_ })
            $state = $j.State
            if (-not $done) { Stop-Job $j; $state = 'TimedOut' }
            Remove-Job $j -Force
            return [ordered]@{ state = [string]$state; output = $out }
        }
        'mkdir' {
            $p = Full $arg
            if (-not (Test-Path -LiteralPath $p)) { New-Item -ItemType Directory -Force $p | Out-Null; Owner-Admins $p }
            return [ordered]@{ path = $p; acl = (Acl $p) }
        }
        'put' {
            $p = Full $arg
            $existed = Test-Path -LiteralPath $p
            $backup = $null
            if ($existed) {
                $backup = $p + '.emerg-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.bak'
                Copy-Item -LiteralPath $p -Destination $backup
                # Write into the existing file: its security descriptor stays as it was.
                $fs = [IO.File]::Open($p, [IO.FileMode]::Truncate, [IO.FileAccess]::Write)
                try { $fs.Write($body, 0, $body.Length) } finally { $fs.Dispose() }
            } else {
                $dir = Split-Path $p -Parent
                if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null; Owner-Admins $dir }
                [IO.File]::WriteAllBytes($p, $body)
                Owner-Admins $p
            }
            return [ordered]@{ path = $p; bytes = $body.Length; sha256 = (Get-FileHash -LiteralPath $p).Hash; replaced = $existed; backup = $backup; acl = (Acl $p) }
        }
        'copy' {
            $s, $d = $arg -split '\|', 2; $s = Full $s; $d = Full $d
            $existed = Test-Path -LiteralPath $d
            Copy-Item -LiteralPath $s -Destination $d -Force
            if (-not $existed) { Owner-Admins $d }
            return [ordered]@{ from = $s; to = $d; sha256 = (Get-FileHash -LiteralPath $d).Hash; acl = (Acl $d) }
        }
        'move' {
            $s, $d = $arg -split '\|', 2; $s = Full $s; $d = Full $d
            if (Test-Path -LiteralPath $d) { throw 'move: destination exists' }
            Move-Item -LiteralPath $s -Destination $d
            return [ordered]@{ from = $s; to = $d; acl = (Acl $d) }
        }
        'delete' {
            $p = Full $arg
            if (Test-Path -LiteralPath $p -PathType Container) {
                if (@(Get-ChildItem -LiteralPath $p -Force).Count) { throw 'delete: directory not empty' }
            }
            Remove-Item -LiteralPath $p -Force
            return [ordered]@{ deleted = $p }
        }
        'list' {
            $p = Full $arg
            return [ordered]@{ path = $p; entries = @(Get-ChildItem -LiteralPath $p -Force | Select-Object -First 500 | ForEach-Object {
                '{0} {1,12} {2:yyyy-MM-ddTHH:mm:ssZ} {3}' -f $(if ($_.PSIsContainer) { 'd' } else { '-' }), $(if ($_.PSIsContainer) { '' } else { $_.Length }), $_.LastWriteTimeUtc, $_.Name }) }
        }
        'acl' { $p = Full $arg; return [ordered]@{ path = $p; acl = (Acl $p) } }
        'tail' {
            $parts = $arg -split '\|', 2
            $p = Full $parts[0]
            $n = 60; if ($parts.Count -gt 1) { $n = [Math]::Min(2000, [int]$parts[1]) }
            return [ordered]@{ path = $p; lines = @(Get-Content -LiteralPath $p -Tail $n | ForEach-Object { [string]$_ }) }
        }
        'usb-boot' {
            # status | windows | linux:<grub entry index>. No restart here (use 'reboot').
            $u = Usb-Stick
            if ($arg -eq 'windows') {
                if (Test-Path $u.efi) { Rename-Item -LiteralPath $u.efi -NewName 'bootx64.off' }
            } elseif ($arg -match '^linux:(\d{1,2})$') {
                $n = $Matches[1]
                $cfg = [IO.File]::ReadAllText($u.grub)
                if ($cfg -notmatch '(?m)^set default=\S+') { throw 'usb: no set default line' }
                if (-not (Test-Path ($u.grub + '.orig'))) { Copy-Item -LiteralPath $u.grub -Destination ($u.grub + '.orig') }
                [IO.File]::WriteAllText($u.grub, ([regex]::Replace($cfg, '(?m)^set default=\S+', "set default=$n")), $utf8)
                if (Test-Path $u.off) { Rename-Item -LiteralPath $u.off -NewName 'bootx64.efi' }
            } elseif ($arg -ne 'status') { throw 'usb-boot: status | windows | linux:<n>' }
            return Usb-State $u
        }
        'reboot' {
            if ($arg -ne 'reboot-now') { throw 'reboot needs arg reboot-now' }
            Log 'reboot requested'
            & shutdown.exe /r /t 5 /d p:0:0 /c 'BC250 emergency channel restart'
            return [ordered]@{ reboot = 'requested in 5 s' }
        }
        default { throw "unknown action $action" }
    }
}

$listener = New-Object Net.HttpListener
$listener.Prefixes.Add('http://+:8722/lab/')
$listener.Start()
Log 'listening on 8722 (protocol v2)'
while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    $code = 200; $out = $null; $action = '-'; $raw = $null
    try {
        $req = $ctx.Request
        if ($req.HttpMethod -ne 'POST' -or $req.ContentLength64 -gt $maxBody -or $req.ContentLength64 -lt 0) { $code = 400; throw 'bad request' }
        $buf = New-Object byte[] $req.ContentLength64
        $read = 0
        while ($read -lt $buf.Length) { $r = $req.InputStream.Read($buf, $read, $buf.Length - $read); if ($r -le 0) { break }; $read += $r }
        if ($read -ne $buf.Length) { $code = 400; throw 'short body' }
        $ts = [string]$req.Headers['X-Ts']; $nonce = [string]$req.Headers['X-Nonce']; $sig = [string]$req.Headers['X-Sig']
        $action = [string]$req.Headers['X-Action']; $arg64 = [string]$req.Headers['X-Arg']
        $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        if ($ts -notmatch '^\d{9,11}$' -or [Math]::Abs($now - [long]$ts) -gt 120) { $code = 401; throw 'stale or missing timestamp' }
        if ($nonce -notmatch '^[0-9a-f]{32}$' -or $seen.ContainsKey($nonce)) { $code = 401; throw 'bad or reused nonce' }
        if ($action -notmatch '^[a-z-]{2,20}$') { $code = 400; throw 'bad action' }
        $bodyHash = Hex ($sha.ComputeHash($buf))
        $expect = Hex ($hmac.ComputeHash($utf8.GetBytes("$ts`n$nonce`n$action`n$arg64`n$bodyHash")))
        if ($sig -ne $expect) { $code = 401; throw 'bad signature' }
        $seen[$nonce] = $now
        foreach ($k in @($seen.Keys)) { if ($now - $seen[$k] -gt 300) { $seen.Remove($k) } }
        $arg = $utf8.GetString([Convert]::FromBase64String($arg64))
        if ($action -eq 'get') {
            $p = Full $arg
            if ((Get-Item -LiteralPath $p).Length -gt $maxBody) { throw 'get: file above 16 MB' }
            $raw = [IO.File]::ReadAllBytes($p)
        } else {
            $out = [ordered]@{ ok = $true; action = $action; result = (Run-Action $action $arg $buf) }
        }
    } catch {
        if ($code -eq 200) { $code = 500 }
        $out = [ordered]@{ ok = $false; action = $action; error = $_.Exception.Message }; $raw = $null
    }
    Log ('{0} {1} {2}' -f $ctx.Request.RemoteEndPoint, $action, $code)
    try {
        if ($null -ne $raw) {
            $ctx.Response.ContentType = 'application/octet-stream'
            $ctx.Response.AddHeader('X-Sha256', (Hex ($sha.ComputeHash($raw))))
            $bytes = $raw
        } else {
            $ctx.Response.ContentType = 'application/json'
            $bytes = $utf8.GetBytes(($out | ConvertTo-Json -Depth 5))
        }
        $ctx.Response.StatusCode = $code
        $ctx.Response.ContentLength64 = $bytes.Length
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
        $ctx.Response.Close()
    } catch { Log ('response failed: ' + $_.Exception.Message) }
}
