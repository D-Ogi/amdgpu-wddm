# LAB side of tools/win/dev-swap (unit A, elevated SSH session, Windows PowerShell 5.1). dev_swap.py on the host
# runs this script through tools/win/target.py; it is not a tool to call by hand.
#
# It changes user-mode files of the active bc250kmd package in the DriverStore and nothing else: no pnputil, no
# device restart, no OS restart, no KMD change. A file that a running process holds is renamed aside to
# <name>.swap-aside-<utc>, and the new file is written under its name (the holder keeps the old image until it ends).
#
#   -Action status    read only. Never refuses: it prints what it can read.
#   -Action swap      writes the files that the host staged in -InDir (manifest.json names them).
#   -Action restore   puts the kept originals back and proves each one by its SHA-256.
#
# -RolesB64 is the role table of the host (base64 of JSON: key, arch, role, name, pe), so the names have one source.
# The state lives on the lab, below -Base: state\state.json and state\orig\<key>. The first swap keeps the installed
# bytes of every role that it changes; a later swap keeps only the roles that are new to the state; restore puts all
# of them back and then deletes the state. A state of another package than the active one is stale: swap and
# restore refuse it, and restore -DropStale deletes it.
#
# Output: free text lines, one 'DEVSWAP-JSON <json>' line with the facts the host reads, and a last line
# 'RESULT PASS|FAIL|REFUSED <reason>'. Exit codes: 0 PASS, 1 FAIL, 2 REFUSED.
#
# -FakeRoot <dir> (host tests only): the registry and device facts come from <dir>\facts.json (the shape of
# kit-direct-r6 lab\direct-where.ps1 -Facts plus 'guard' and 'held'), the guard files from <dir>\guard, and no
# takeown or icacls runs. Everything else is the same code.
param(
    [Parameter(Mandatory = $true)][ValidateSet('status', 'swap', 'restore')][string]$Action,
    [Parameter(Mandatory = $true)][string]$RolesB64,
    [string]$Base = 'C:\BC250\dev-swap',
    [string]$InDir = '',
    [switch]$DropStale,
    [string]$FakeRoot = ''
)
$ErrorActionPreference = 'Stop'
$service = 'bc250kmd'
$machines = @{ x64 = 0x8664; x86 = 0x14C }
# The order of a swap: the images that others pin or load come first, the shells that load them last. Restore runs
# it backwards.
$order = @('d3d11-icd', 'd3d12-icd', 'dxvk', 'vkd3d', 'd3d11-record', 'd3d11-shell', 'd3d12-shell')

function Result([string]$verdict, [string]$reason) {
    "RESULT $verdict $reason"
    if ($verdict -eq 'PASS') { exit 0 }
    if ($verdict -eq 'REFUSED') { exit 2 }
    exit 1
}
# SHA-256 through .NET with shared access, so that a file which a process holds can be read, and no module loads.
function Sha([string]$p) {
    $fs = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $h = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($fs)) -replace '-', '') } finally { $h.Dispose(); $fs.Dispose() }
}
function Utc { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') }

# The PE machine word, 0 for a file that is not a PE image.
function Machine([string]$p) {
    $fs = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $h = New-Object byte[] 4096
        $n = $fs.Read($h, 0, $h.Length)
    } finally { $fs.Dispose() }
    if ($n -lt 64 -or $h[0] -ne 0x4D -or $h[1] -ne 0x5A) { return 0 }
    $at = [BitConverter]::ToInt32($h, 0x3C)
    if ($at -le 0 -or $at + 6 -gt $n -or $h[$at] -ne 0x50 -or $h[$at + 1] -ne 0x45 -or $h[$at + 2] -ne 0 -or $h[$at + 3] -ne 0) { return 0 }
    return [int][BitConverter]::ToUInt16($h, $at + 4)
}

# The Windows catalog image identity (CryptCATAdminCalcHashFromFileHandle2, SHA256): the pin of the D3D11 record and
# the hash that the D3D11 shell checks. The same function as tools/release/image_identity.py.
# PROVENANCE: Microsoft WDK mscat.h, CryptCATAdminCalcHashFromFileHandle2.
if (-not ('DevSwapImage' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class DevSwapImage {
    [DllImport("wintrust.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CryptCATAdminAcquireContext2(out IntPtr context, IntPtr subsystem, string algorithm, IntPtr policy, uint flags);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr context, SafeFileHandle file, ref uint size, byte[] hash, uint flags);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern bool CryptCATAdminReleaseContext(IntPtr context, uint flags);
    public static string Digest(string path) {
        IntPtr context;
        if (!CryptCATAdminAcquireContext2(out context, IntPtr.Zero, "SHA256", IntPtr.Zero, 0))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
                uint size = 32;
                byte[] hash = new byte[32];
                if (!CryptCATAdminCalcHashFromFileHandle2(context, stream.SafeFileHandle, ref size, hash, 0))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                if (size != 32) throw new InvalidOperationException("unexpected catalog SHA-256 size");
                return BitConverter.ToString(hash).Replace("-", "");
            }
        } finally { CryptCATAdminReleaseContext(context, 0); }
    }
}
'@
}
function Digest([string]$p) { if ((Machine $p) -eq 0) { return $null }; return [DevSwapImage]::Digest($p) }

# The engine and ICD pins of a v3 D3D11 record, or the reason it is not one (tools/release/check_package_v3.py).
function RecordPins([byte[]]$b) {
    if ($b.Length -ne 132) { return @{ ok = $false; why = "$($b.Length) bytes, a v3 record is 132" } }
    $magic = [BitConverter]::ToUInt32($b, 0); $ver = [BitConverter]::ToUInt32($b, 4); $size = [BitConverter]::ToUInt32($b, 8)
    if ($magic -ne 0x4334314D -or $ver -ne 3 -or $size -ne 132 -or [BitConverter]::ToUInt32($b, 12) -ne 0) {
        return @{ ok = $false; why = ('header magic 0x{0:x8} version {1} size {2}, a v3 record is 0x4334314d 3 132' -f $magic, $ver, $size) }
    }
    $e = -join ($b[68..99] | ForEach-Object { $_.ToString('X2') })
    $i = -join ($b[100..131] | ForEach-Object { $_.ToString('X2') })
    return @{ ok = $true; engine = $e; icd = $i }
}

# ---- facts: the active package (kit-direct-r6 lab\direct-where.ps1 part 2), the guard, the holders ----
function Get-LiveFacts {
    $f = @{ repo = (Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'); imagePath = $null; adapters = @(); infFiles = @{} }
    $svc = Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Services\$service" -ErrorAction SilentlyContinue
    if ($svc) { $f.imagePath = [string]$svc.ImagePath }
    foreach ($d in @(Get-PnpDevice -Class Display -PresentOnly -ErrorAction SilentlyContinue)) {
        $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service, DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue
        $svcName = [string]($p | Where-Object KeyName -eq 'DEVPKEY_Device_Service').Data
        $inf = [string]($p | Where-Object KeyName -eq 'DEVPKEY_Device_DriverInfPath').Data
        $f.adapters += @{ service = $svcName; inf = $inf }
        if ($svcName -eq $service -and $inf -and -not $f.infFiles.ContainsKey($inf)) {
            $pk = @(Get-WindowsDriver -Online -ErrorAction SilentlyContinue | Where-Object { [string]$_.Driver -ieq $inf } |
                ForEach-Object { Split-Path -Leaf (Split-Path -Parent ([string]$_.OriginalFileName)) } | Select-Object -Unique)
            if ($pk.Count -eq 1) { $f.infFiles[$inf] = @{ packages = @($pk); active = $pk[0] } }
        }
    }
    return $f
}
function Get-FileFacts($j) {
    $f = @{ repo = [string]$j.repo; imagePath = $j.imagePath; adapters = @(); infFiles = @{} }
    foreach ($a in @($j.adapters)) { if ($a) { $f.adapters += @{ service = [string]$a.service; inf = [string]$a.inf } } }
    if ($j.infFiles) { foreach ($p in $j.infFiles.PSObject.Properties) { $f.infFiles[$p.Name] = @{ packages = @($p.Value.packages); active = $p.Value.active } } }
    return $f
}
function Package-FromImagePath([string]$imagePath) {
    if (-not $imagePath) { return $null }
    $p = $imagePath.Trim('"')
    $marker = '\DriverStore\FileRepository\'
    $i = $p.IndexOf($marker, [StringComparison]::OrdinalIgnoreCase)
    if ($i -lt 0) { return $null }
    $parts = $p.Substring($i + $marker.Length).Split('\')
    if ($parts.Count -ne 2 -or $parts[1] -ne "$service.sys" -or $parts[0] -notlike "$service.inf_*") { return $null }
    return $parts[0]
}
# The active package directory name, or a reason in .problem: the service ImagePath and every present adapter of
# the service must name the same package.
function ActivePackage($f) {
    $fromService = Package-FromImagePath $f.imagePath
    if (-not $fromService) { return @{ problem = "the $service service names no DriverStore package" } }
    $seen = @()
    foreach ($a in $f.adapters) {
        if ($a.service -ne $service) { continue }
        $e = $f.infFiles[$a.inf]; $pkg = ''
        if ($e) { if ($e.active) { $pkg = [string]$e.active } elseif (@($e.packages).Count -eq 1) { $pkg = [string]@($e.packages)[0] } }
        $seen += $pkg
    }
    if (-not $seen.Count) { return @{ problem = "no present display adapter runs the $service service" } }
    $other = @($seen | Where-Object { $_ -ne $fromService })
    if ($other.Count) { return @{ problem = "the adapter package ($($seen -join ', ')) is not the service package $fromService (a restart pending?)" } }
    $dir = Join-Path $f.repo $fromService
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return @{ problem = "no directory for the package $fromService" } }
    return @{ name = $fromService; dir = $dir }
}
# The HLK campaign guard, read from its files with shared read, write and delete access. guard.ps1 -Action Status is
# never called: it opens guard.jsonl with read sharing only, which blocks the guard's log writer and trips the guard.
function GuardFacts {
    $g = @{ service = ''; trip = '' }
    if ($FakeRoot) { $g.service = [string]$script:fake.guard; $dir = Join-Path $FakeRoot 'guard' }
    else {
        $s = Get-Service HlkCampaignGuard -ErrorAction SilentlyContinue
        if ($s) { $g.service = "$($s.Status)" }
        $dir = Join-Path $env:ProgramFiles 'BC250-HLK-Guard'
    }
    $tp = Join-Path $dir 'trip.json'
    if ([IO.File]::Exists($tp)) {
        $fs = New-Object IO.FileStream($tp, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try { $t = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
        $g.trip = if ($t -match '"reason"\s*:\s*"([^"]+)"') { $Matches[1] } elseif ($t.Trim() -and $t.Trim() -ne 'null') { 'unreadable' } else { '' }
    }
    return $g
}
# The full paths (lower case) that a running process holds as a module, for the names of the role table.
function HeldPaths([hashtable]$byName) {
    $held = @{}
    if ($FakeRoot) {
        foreach ($n in @($script:fake.held)) { if ($byName.ContainsKey("$n".ToLowerInvariant())) { $held[$byName["$n".ToLowerInvariant()].ToLowerInvariant()] = 1 } }
        return $held
    }
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
        try { foreach ($m in $p.Modules) { if ($byName.ContainsKey($m.ModuleName.ToLowerInvariant())) { $held[$m.FileName.ToLowerInvariant()] = 1 } } } catch { }
    }
    return $held
}

# ---- state ----
$stateDir = Join-Path $Base 'state'
$stateFile = Join-Path $stateDir 'state.json'
$origDir = Join-Path $stateDir 'orig'
function KeepPath([string]$key) { Join-Path $origDir (($key -replace '[^A-Za-z0-9_-]', '_') + '.bin') }
function ReadState {
    if (-not (Test-Path -LiteralPath $stateFile)) { return $null }
    $j = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
    $s = @{ package = [string]$j.package; created = [string]$j.created; roles = [ordered]@{} }
    if ($j.roles) { foreach ($p in $j.roles.PSObject.Properties) { $s.roles[$p.Name] = @{ path = [string]$p.Value.path; sha256 = [string]$p.Value.sha256 } } }
    return $s
}
function WriteState($s) {
    New-Item -ItemType Directory -Force $stateDir | Out-Null
    $tmp = "$stateFile.tmp"
    [IO.File]::WriteAllText($tmp, ($s | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp -Destination $stateFile -Force
}

# One write of a DriverStore file: ownership and the write right first (a DriverStore file is owned by
# TrustedInstaller), a held file renamed aside, the copy, the proof by hash. A failed copy puts the renamed file back,
# so a name is never left empty. Returns $null or the reason of the failure.
function WriteFile([string]$path, [string]$src, [string]$want, [hashtable]$held) {
    $prior = $ErrorActionPreference
    if (-not $FakeRoot) {
        $ErrorActionPreference = 'Continue'
        & takeown.exe /f $path /a 2>$null | Out-Null
        & icacls.exe $path /grant '*S-1-5-32-544:F' 2>$null | Out-Null
        $ErrorActionPreference = $prior
    }
    $aside = $null
    if ($held.ContainsKey($path.ToLowerInvariant())) {
        # A new file under the old name needs the create right on the directory.
        $dir = Split-Path -Parent $path
        if (-not $FakeRoot) {
            $ErrorActionPreference = 'Continue'
            & takeown.exe /f $dir /a 2>$null | Out-Null
            & icacls.exe $dir /grant '*S-1-5-32-544:(M)' 2>$null | Out-Null
            $ErrorActionPreference = $prior
        }
        $aside = "$path.swap-aside-$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))"
        try { Move-Item -LiteralPath $path -Destination $aside } catch { return "it is loaded and could not be renamed aside ($($_.Exception.Message.Trim()))" }
        "  renamed the loaded copy aside: $(Split-Path -Leaf $aside)"
    }
    try { Copy-Item -LiteralPath $src -Destination $path -Force }
    catch {
        $why = "it could not be written ($($_.Exception.Message.Trim()))"
        if ($aside -and -not (Test-Path -LiteralPath $path)) { Move-Item -LiteralPath $aside -Destination $path; $why += '; the renamed copy is back' }
        return $why
    }
    $got = Sha $path
    if ($got -ne $want) { return "it holds $got after the copy, $want expected" }
    return $null
}
# Aside files of an earlier run that nothing holds any more.
function DropAside([string[]]$paths) {
    foreach ($p in $paths) {
        foreach ($a in @(Get-ChildItem -LiteralPath (Split-Path -Parent $p) -File -Filter "$(Split-Path -Leaf $p).swap-aside-*" -ErrorAction SilentlyContinue)) {
            try { Remove-Item -LiteralPath $a.FullName -Force; "removed old aside $($a.Name)" } catch { "old aside still held, left $($a.Name)" }
        }
    }
}

# ---- common part ----
$roles = @()
# Windows PowerShell 5.1 writes a JSON array to the pipeline as one object; the assignment and foreach unroll it.
try { $parsed = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($RolesB64)) | ConvertFrom-Json; $roles = @(foreach ($x in $parsed) { $x }) } catch { Result 'REFUSED' "-RolesB64 is not base64 JSON ($($_.Exception.Message))" }
if (-not $roles.Count) { Result 'REFUSED' 'the role table is empty' }
# Plain strings: a value of ConvertFrom-Json used as a dictionary key is a PSObject, which ConvertTo-Json refuses.
$roles = @($roles | ForEach-Object { [pscustomobject]@{ key = [string]$_.key; arch = [string]$_.arch; role = [string]$_.role; name = [string]$_.name; pe = [bool]$_.pe } })
$script:fake = $null
if ($FakeRoot) { $script:fake = Get-Content -LiteralPath (Join-Path $FakeRoot 'facts.json') -Raw | ConvertFrom-Json }
"utc $(Utc) action $Action"
$facts = if ($FakeRoot) { Get-FileFacts $script:fake } else { Get-LiveFacts }
$pkg = ActivePackage $facts
$guard = GuardFacts
$state = ReadState
"package $(if ($pkg.name) { $pkg.name } else { "unknown: $($pkg.problem)" })"
"guard service '$($guard.service)' trip '$($guard.trip)'"

# Every role of the table in the active package: exactly one file of that name, and for an image, of that machine.
$files = @{}
$byName = @{}
$where = [ordered]@{}
if ($pkg.name) {
    foreach ($f in @(Get-ChildItem -LiteralPath $pkg.dir -Recurse -File -ErrorAction SilentlyContinue)) {
        $n = $f.Name.ToLowerInvariant()
        if (-not $files.ContainsKey($n)) { $files[$n] = @() }
        $files[$n] += $f.FullName
    }
    foreach ($r in $roles) {
        $n = $r.name.ToLowerInvariant()
        $c = @(); if ($files.ContainsKey($n)) { $c = @($files[$n]) }
        if ($r.pe) { $c = @($c | Where-Object { (Machine $_) -eq $machines[$r.arch] }) }
        if ($c.Count -eq 1) { $where[$r.key] = $c[0]; $byName[$n] = $c[0] }
        else { $where[$r.key] = "$($c.Count) copies" }
    }
}
function RoleFacts([string]$key) {
    $p = $where[$key]
    $o = [ordered]@{ found = $false }
    if (-not $p -or -not (Test-Path -LiteralPath $p -PathType Leaf)) { $o.problem = if ($p) { "$p in the package" } else { 'package unknown' }; return $o }
    $o.found = $true
    $o.path = $p.Substring($pkg.dir.Length).TrimStart('\')
    $o.sha256 = Sha $p
    $o.machine = Machine $p
    if ($o.machine) { $o.digest = Digest $p }
    else {
        $rp = RecordPins ([IO.File]::ReadAllBytes($p))
        if ($rp.ok) { $o.pins = @($rp.engine, $rp.icd) } else { $o.record_problem = $rp.why }
        $o.hex = -join ([IO.File]::ReadAllBytes($p) | ForEach-Object { $_.ToString('X2') })
    }
    if ($state -and $state.roles.Contains($key)) { $o.kept_sha256 = $state.roles[$key].sha256 }
    return $o
}
$report = [ordered]@{ action = $Action; package = $pkg.name; package_problem = $pkg.problem; guard = $guard; roles = [ordered]@{} }
if ($state) { $report.state = [ordered]@{ package = $state.package; created = $state.created; roles = @($state.roles.Keys); stale = ($state.package -ne $pkg.name) } }
foreach ($r in $roles) { $report.roles[$r.key] = RoleFacts $r.key }

if ($Action -eq 'status') {
    "DEVSWAP-JSON $($report | ConvertTo-Json -Depth 6 -Compress)"
    Result 'PASS' 'status read'
}

# ---- swap and restore refuse on these ----
"DEVSWAP-JSON $($report | ConvertTo-Json -Depth 6 -Compress)"
if (-not $pkg.name) { Result 'REFUSED' "the active package is not known: $($pkg.problem)" }
if ($guard.service -eq 'Running') { Result 'REFUSED' 'the HLK campaign guard service runs (an HLK campaign is armed)' }
if ($guard.trip) { Result 'REFUSED' "the HLK campaign guard has a trip ($($guard.trip))" }
foreach ($r in $roles) { if (-not $report.roles[$r.key].found) { Result 'REFUSED' "$($r.key) $($r.name): $($report.roles[$r.key].problem)" } }

if ($Action -eq 'restore') {
    if (-not $state) { Result 'PASS' 'nothing is swapped, nothing to restore' }
    if ($state.package -ne $pkg.name) {
        if ($DropStale) { Remove-Item -LiteralPath $stateDir -Recurse -Force; Result 'PASS' "dropped the stale state of package $($state.package)" }
        Result 'REFUSED' "the state belongs to package $($state.package), the active package is $($pkg.name) (restore -DropStale deletes it)"
    }
    $keys = @($state.roles.Keys)
    foreach ($k in $keys) {
        $kp = KeepPath $k
        if (-not (Test-Path -LiteralPath $kp) -or (Sha $kp) -ne $state.roles[$k].sha256) { Result 'FAIL' "$k`: the kept original is missing or changed; nothing was written" }
    }
    $paths = @($keys | ForEach-Object { $where[$_] })
    DropAside $paths
    $held = HeldPaths $byName
    $done = 0; $proved = 0
    $ordered = @($order | ForEach-Object { $role = $_; $keys | Where-Object { ($_ -split ':')[1] -eq $role } })
    [array]::Reverse($ordered)
    foreach ($k in $ordered) {
        $p = $where[$k]; $want = $state.roles[$k].sha256
        if ((Sha $p) -ne $want) {
            $out = @(WriteFile $p (KeepPath $k) $want $held)
            foreach ($l in $out) { if ("$l".StartsWith('  ')) { $l } }
            $why = @($out | Where-Object { $_ -and -not "$_".StartsWith('  ') })
            if ($why.Count) { Result 'FAIL' "$k`: $($why[0]) (the state stays; restore again)" }
            $done++
        }
        if ((Sha $p) -eq $want) { $proved++; "restored $k $want" } else { Result 'FAIL' "$k holds $(Sha $p), the original is $want" }
    }
    Remove-Item -LiteralPath $stateDir -Recurse -Force
    Result 'PASS' "restored $done, proved $proved of $($keys.Count) by SHA-256, state deleted"
}

# ---- swap ----
if ($state -and $state.package -ne $pkg.name) { Result 'REFUSED' "the state belongs to package $($state.package), the active package is $($pkg.name) (restore -DropStale deletes it)" }
if (-not $InDir -or -not (Test-Path -LiteralPath (Join-Path $InDir 'manifest.json'))) { Result 'REFUSED' 'no staged manifest.json in -InDir' }
$man = Get-Content -LiteralPath (Join-Path $InDir 'manifest.json') -Raw | ConvertFrom-Json
if ([string]$man.package -ne $pkg.name) { Result 'REFUSED' "the host planned for package $($man.package), the active package is $($pkg.name)" }
$new = @{}
foreach ($m in @($man.files)) {
    $r = @($roles | Where-Object { $_.key -eq $m.key })
    if (-not $r.Count) { Result 'REFUSED' "the manifest names $($m.key), which is not in the role table" }
    $src = Join-Path $InDir ([string]$m.file)
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { Result 'REFUSED' "$($m.key): the staged file $($m.file) is missing" }
    if ((Sha $src) -ne ([string]$m.sha256).ToUpperInvariant()) { Result 'REFUSED' "$($m.key): the staged file is $(Sha $src), the host sent $($m.sha256)" }
    if ($r[0].pe -and (Machine $src) -ne $machines[$r[0].arch]) { Result 'REFUSED' "$($m.key): the staged file is not a $($r[0].arch) image" }
    $new[$m.key] = $src
}
if (-not $new.Count) { Result 'REFUSED' 'the manifest names no file' }

# The records: for each architecture whose engine, ICD or record changes, the installed record must pin the
# installed engine and ICD (positive control), and the record after the swap must pin the engine and ICD after the
# swap, with the same first 68 bytes (the capabilities never change here).
foreach ($arch in @('x64', 'x86')) {
    $ek = "${arch}:dxvk"; $ik = "${arch}:d3d11-icd"; $rk = "${arch}:d3d11-record"
    if (-not ($new.ContainsKey($ek) -or $new.ContainsKey($ik) -or $new.ContainsKey($rk))) { continue }
    if (-not ($where.Contains($ek) -and $where.Contains($ik) -and $where.Contains($rk))) { Result 'REFUSED' "$arch`: the role table has no dxvk, d3d11-icd and d3d11-record" }
    if (($new.ContainsKey($ek) -or $new.ContainsKey($ik)) -and -not $new.ContainsKey($rk)) { Result 'REFUSED' "$arch`: the engine or the ICD changes and the manifest carries no new record" }
    $inst = RecordPins ([IO.File]::ReadAllBytes($where[$rk]))
    if (-not $inst.ok) { Result 'REFUSED' "$rk installed: $($inst.why)" }
    $ie = Digest $where[$ek]; $ii = Digest $where[$ik]
    if ($inst.engine -ne $ie) { Result 'REFUSED' "$rk installed pins engine $($inst.engine.Substring(0,8)), the installed engine is $($ie.Substring(0,8)) (positive control)" }
    if ($inst.icd -ne $ii) { Result 'REFUSED' "$rk installed pins ICD $($inst.icd.Substring(0,8)), the installed ICD is $($ii.Substring(0,8)) (positive control)" }
    $fe = if ($new.ContainsKey($ek)) { Digest $new[$ek] } else { $ie }
    $fi = if ($new.ContainsKey($ik)) { Digest $new[$ik] } else { $ii }
    $nb = [IO.File]::ReadAllBytes($new[$rk])
    $nr = RecordPins $nb
    if (-not $nr.ok) { Result 'REFUSED' "$rk new: $($nr.why)" }
    if ($nr.engine -ne $fe -or $nr.icd -ne $fi) { Result 'REFUSED' "$rk new pins engine $($nr.engine.Substring(0,8)) ICD $($nr.icd.Substring(0,8)), the files after the swap are $($fe.Substring(0,8)) $($fi.Substring(0,8))" }
    $ib = [IO.File]::ReadAllBytes($where[$rk])
    for ($x = 0; $x -lt 68; $x++) { if ($ib[$x] -ne $nb[$x]) { Result 'REFUSED' "$rk new: byte $x differs from the installed record (the capabilities never change in a swap)" } }
    "record $arch control PASS: installed pins = installed engine $($ie.Substring(0,8)) ICD $($ii.Substring(0,8)); after the swap engine $($fe.Substring(0,8)) ICD $($fi.Substring(0,8))"
}

# Keep the installed bytes of every role that is new to the state, before the first write.
if (-not $state) { $state = @{ package = $pkg.name; created = (Utc); roles = [ordered]@{} } }
New-Item -ItemType Directory -Force $origDir | Out-Null
foreach ($k in @($new.Keys | Sort-Object)) {
    if ($state.roles.Contains($k)) { continue }
    $kp = KeepPath $k
    Copy-Item -LiteralPath $where[$k] -Destination $kp -Force
    $h = Sha $where[$k]
    if ((Sha $kp) -ne $h) { Result 'FAIL' "$k`: the kept copy does not match the installed file; nothing was written" }
    $state.roles[$k] = @{ path = $report.roles[$k].path; sha256 = $h }
    "kept $k $h"
}
WriteState $state

$paths = @($new.Keys | ForEach-Object { $where[$_] })
DropAside $paths
$held = HeldPaths $byName
$written = 0; $same = 0
foreach ($role in $order) {
    foreach ($k in @($new.Keys | Where-Object { ($_ -split ':')[1] -eq $role } | Sort-Object)) {
        $p = $where[$k]; $want = Sha $new[$k]; $before = Sha $p
        if ($before -eq $want) { "already $k $want"; $same++; continue }
        $out = @(WriteFile $p $new[$k] $want $held)
        foreach ($l in $out) { if ("$l".StartsWith('  ')) { $l } }
        $why = @($out | Where-Object { $_ -and -not "$_".StartsWith('  ') })
        if ($why.Count) { Result 'FAIL' "$k`: $($why[0]) ($written written; the state is kept, restore puts the originals back)" }
        "swapped $k $before -> $want"
        $written++
    }
}
Remove-Item -LiteralPath $InDir -Recurse -Force -ErrorAction SilentlyContinue
Result 'PASS' "written $written, already in place $same, kept originals for $($state.roles.Count) roles"
