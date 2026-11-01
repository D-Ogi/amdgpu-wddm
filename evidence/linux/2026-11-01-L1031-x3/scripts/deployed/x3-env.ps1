# Pure env parsing and KMD snapshot policy; Windows PowerShell 5.1.
# No IO, registry, process or hardware access on import.
Set-StrictMode -Version 2

function Read-X3Env([byte[]]$Bytes) {
    if ($null -eq $Bytes -or $Bytes.Length -ne 1024) { throw 'grubenv must be exactly 1024 bytes' }
    foreach ($b in $Bytes) {
        if ($b -ne 10 -and ($b -lt 32 -or $b -gt 126)) { throw 'grubenv has a non-ASCII/control byte' }
    }
    $text = [Text.Encoding]::ASCII.GetString($Bytes)
    $header = "# GRUB Environment Block`n"
    if (-not $text.StartsWith($header, [StringComparison]::Ordinal)) { throw 'invalid grubenv header' }
    $rest = $text.Substring($header.Length)
    $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $lines = New-Object 'System.Collections.Generic.List[string]'
    while (-not ($rest -cmatch '^#+$')) {
        $m = [regex]::Match($rest, '\A([A-Za-z_][A-Za-z0-9_]*)=([^\r\n]*)\n')
        if (-not $m.Success) { throw 'invalid grubenv variable or padding' }
        if (-not $names.Add($m.Groups[1].Value)) { throw 'duplicate grubenv variable' }
        if ($m.Groups[1].Value -ceq 'bc250_try' -and $m.Groups[2].Value -cnotmatch '^[123]$') { throw 'invalid bc250_try value' }
        $lines.Add($m.Value)
        $rest = $rest.Substring($m.Length)
    }
    return ,$lines.ToArray()
}

function Clear-X3Env([byte[]]$Bytes) {
    $lines = Read-X3Env $Bytes
    $text = "# GRUB Environment Block`n"
    foreach ($line in $lines) { if (-not $line.StartsWith('bc250_try=', [StringComparison]::Ordinal)) { $text += $line } }
    if ($text.Length -ge 1024) { throw 'grubenv has no padding space' }
    return ,[Text.Encoding]::ASCII.GetBytes($text + ('#' * (1024 - $text.Length)))
}

function Read-X3Health([string]$Output, [int]$ExitCode) {
    if ($ExitCode -ne 0) { throw "health reader exit $ExitCode" }
    $rx = '(?m)^health abi=(\d+) version=0x([0-9A-Fa-f]{8}) flags=(\d+) generation=(\d+) epoch=(\d+) completed=(\d+) age_ms=(\d+) ready_ms=(\d+)\r?$'
    $all = [regex]::Matches($Output, $rx)
    if ($all.Count -ne 1) { throw 'expected exactly one complete health snapshot' }
    $m = $all[0]
    $h = [pscustomobject]@{ Abi=[uint64]$m.Groups[1].Value; Version=$m.Groups[2].Value;
        Flags=[uint64]$m.Groups[3].Value; Generation=[uint64]$m.Groups[4].Value;
        Epoch=[uint64]$m.Groups[5].Value; Completed=[uint64]$m.Groups[6].Value;
        AgeMs=[uint64]$m.Groups[7].Value; ReadyMs=[uint64]$m.Groups[8].Value }
    # Same contract as KMD start_health.c HealthConfirmable and train preflight.
    # Only reads an existing confirmation; no health-confirm operation.
    if ($h.Abi -ne 1 -or $h.Flags -notin @(15,31) -or $h.Generation -eq 0 -or
        $h.Epoch -eq 0 -or $h.Completed -eq 0 -or $h.ReadyMs -lt 60000 -or
        ($h.AgeMs -gt 15000 -and ($h.Flags -band 16) -eq 0)) { throw 'KMD start is not confirmed and healthy' }
    return $h
}

function Assert-X3HealthPair($First, $Second) {
    if ($First.Generation -ne $Second.Generation -or $First.Epoch -ne $Second.Epoch -or
        $First.Version -cne $Second.Version -or $Second.Completed -lt $First.Completed -or
        $Second.ReadyMs -lt $First.ReadyMs) { throw 'KMD changed or regressed during health observation' }
}

function Assert-X3DesktopPair($First, $Second, [string]$BootUtc) {
    foreach ($s in @($First, $Second)) {
        if ($s.Schema -ne 1 -or $s.BootUtc -cne $BootUtc -or $s.SessionId -le 0 -or
            $s.ShellPid -le 0 -or $s.DwmPid -le 0 -or $s.ShellStart -le 0 -or $s.DwmStart -le 0 -or
            $s.ShellResponsive -ne $true -or $s.DwmFlushed -ne $true) { throw 'desktop witness is not live in this boot' }
        if ($s.ForceCpu -eq 0) {
            if ($s.Zink -ne $true) { throw 'GPU route not actually loaded' }
        } elseif ($s.ForceCpu -eq 1) {
            if ($s.Zink -ne $false -or $s.CpuRenderer -ne $true) { throw 'intentional CPU route not actually loaded' }
        } else { throw 'unknown route intent' }
    }
    foreach ($field in @('BootUtc','SessionId','ShellPid','ShellStart','DwmPid','DwmStart','ForceCpu')) {
        if ($First.$field -cne $Second.$field) { throw "desktop changed during observation: $field" }
    }
    return $(if ($Second.ForceCpu -eq 1) { 'cpu' } else { 'gpu' })
}

function Assert-X3Boot($Desktop1, $Desktop2, [string]$BootUtc, [bool]$SshServiceRunning, [bool]$EmergencyTaskRunning, $Health1, $Health2) {
    # Local status preconditions only. Network handshakes remain a separate gate.
    if (-not $SshServiceRunning -or -not $EmergencyTaskRunning) { throw 'recovery service/task is not Running' }
    $route = Assert-X3DesktopPair $Desktop1 $Desktop2 $BootUtc
    if ($route -eq 'gpu') {
        if ($null -eq $Health1 -or $null -eq $Health2) { throw 'GPU route requires current confirmed KMD health' }
        Assert-X3HealthPair $Health1 $Health2
    }
    # Explicit CPU recovery does not require FULL/READY/VISIBLE/CONFIRMED from KMD.
    return $route
}
