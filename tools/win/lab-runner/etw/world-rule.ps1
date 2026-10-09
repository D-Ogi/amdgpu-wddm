# The world signal of a game trial (BD-107), as one rule in one place.
#
# Why this file exists. Measurement window B must open on the game's world, not on its menu. The harness used to
# decide that from a picture: the menu pass of the game runtime compared a 6x8 brightness grid of two screenshots
# and called a changed grid a "transition candidate". The Witcher 3 Remaster main menu is a lit 3D scene, so it
# passes that test, and in the b26 validation three of four world arms (trials 520, 522, 523) measured the menu or
# the loading screen instead of the world. The fourth (519) escaped only because the menu was dismissed within
# seconds. The frame-time comparison the session was run for could not be made at all.
#
# What the rule reads instead. The trial already samples the kernel driver's own governor once a second
# (scratch\dpm\trial-samplers.ps1 -> C:\BC250\tmp\dpm-<trial>.txt, the lines of `bc250kmd_cli dpm N MS`). Those
# lines carry the shader clock and the GPU busy fraction, and the two screens are far apart on the lab:
#
#   The Witcher 3 Remaster main menu   1000 MHz (the DPM floor), busy 0-40 %, presenting at the 240 fps cap
#   the world, HIGH with ray tracing   1100-1500 MHz, busy 93-100 %, presenting at 12-35 fps
#
# So "the world" is: the clock above the floor AND the busy fraction at least -MinBusy, held for -MinRunSamples
# samples and for at least -SettleSeconds. The loading screen holds the GPU the same way as the world, so the
# settle time is what keeps a window off the first seconds of a load; the picture itself stays with the operator,
# whose explicit mark on the control channel overrides this rule in both directions.
#
# The rule refuses rather than guesses: no telemetry, a stale file or a menu band all return world=$false with a
# reason. A caller that would otherwise have measured something (the time-bounded fallback of etw-capture.ps1)
# says in its notes which reason stopped it.
#
# Dot-source it; it defines functions and runs nothing. Windows PowerShell 5.1 (the lab's shell) and pwsh both
# parse it. Its checks are in host-checks.ps1 next to this file, including the four recorded b26 moments.

# One sampler line, as the kernel driver's CLI writes it (lab local time, no date):
#   16:27:27.624 dpm 1000 MHz  820 mV (SMU 1000 MHz VID 116)  68.4 C busy  13.3% avg ...
# 'fixed' instead of 'dpm' is the legacy fixed-clock form of the same sampler.
$script:Bc250WorldSampleLine = '^(?<t>\d\d:\d\d:\d\d\.\d+) (?:dpm|fixed)\s+(?<mhz>\d+) MHz.*? busy\s+(?<busy>[\d.]+)%'

# Parses sampler text into ascending samples with a date taken from $Reference (the lines carry a time of day
# only). A line whose time of day is more than 12 h ahead of the reference belongs to the day before, so a
# session that runs across midnight still reads in order.
function Get-WorldSamples {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text, [datetime]$Reference = [DateTime]::Now)
    $out = @()
    foreach ($line in ($Text -split "`n")) {
        $m = [regex]::Match($line, $script:Bc250WorldSampleLine)
        if (!$m.Success) { continue }
        $parsed = [datetime]::MinValue
        if (![datetime]::TryParseExact($m.Groups['t'].Value, 'HH:mm:ss.fff',
                [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$parsed)) { continue }
        $time = $Reference.Date.Add($parsed.TimeOfDay)
        if (($time - $Reference).TotalHours -gt 12) { $time = $time.AddDays(-1) }
        $out += , [pscustomobject]@{ time = $time; mhz = [int]$m.Groups['mhz'].Value; busy = [double]$m.Groups['busy'].Value }
    }
    return @($out | Sort-Object time)
}

# The decision. $Samples is what Get-WorldSamples returned, $Now the moment being judged (lab local time).
# Samples after $Now are ignored, so a recorded trial can be replayed moment by moment.
#   -MenuMHz        the DPM floor the menu sits on; the world needs a clock above it (1000 on this lab)
#   -MinBusy        the busy fraction the world holds (the lab reads 93-100 % in the world, 0-40 % in the menu)
#   -MinRunSamples  how many consecutive samples must be in the band (guards a 250 ms sampler interval)
#   -SettleSeconds  how long the band must have held; the loading screen is in the band too
#   -FreshSeconds   how old the newest sample may be before the telemetry counts as stale
# Returns @{ world; why; telemetry; band_since; band_seconds; run; mhz; busy; age_seconds }.
function Test-WorldSignal {
    param([object[]]$Samples, [datetime]$Now = [DateTime]::Now, [int]$MenuMHz = 1000, [double]$MinBusy = 90,
        [int]$MinRunSamples = 8, [int]$SettleSeconds = 25, [int]$FreshSeconds = 15)
    $r = [ordered]@{ world = $false; why = ''; telemetry = $false; band_since = $null; band_seconds = 0.0
        run = 0; mhz = 0; busy = 0.0; age_seconds = $null }
    $seen = @($Samples | Where-Object { $_ -and $_.time -le $Now })
    if (!$seen.Count) { $r.why = 'no telemetry'; return $r }
    $r.telemetry = $true
    $last = $seen[-1]
    $r.mhz = [int]$last.mhz; $r.busy = [double]$last.busy
    $r.age_seconds = [Math]::Round(($Now - $last.time).TotalSeconds, 1)
    if ($r.age_seconds -gt $FreshSeconds) {
        $r.why = "telemetry stale $($r.age_seconds) s"; return $r
    }
    # The run of in-band samples that ends at the newest one. Counted in samples, so a missed second does not
    # end it, and measured in seconds from its first sample, which is what -SettleSeconds judges.
    $run = 0; $since = $null
    for ($i = $seen.Count - 1; $i -ge 0; $i--) {
        if ($seen[$i].mhz -gt $MenuMHz -and $seen[$i].busy -ge $MinBusy) { $run++; $since = $seen[$i].time; continue }
        break
    }
    $r.run = $run
    if (!$run) {
        $r.why = ('menu band: {0} MHz busy {1:0.0}%' -f $r.mhz, $r.busy); return $r
    }
    $r.band_since = $since
    $r.band_seconds = [Math]::Round(($Now - $since).TotalSeconds, 1)
    if ($run -lt $MinRunSamples) {
        $r.why = "band only $run samples (needs $MinRunSamples)"; return $r
    }
    if ($r.band_seconds -lt $SettleSeconds) {
        $r.why = ('band held {0:0.0} s (needs {1} s)' -f $r.band_seconds, $SettleSeconds); return $r
    }
    $r.world = $true
    $r.why = ('world band {0:0.0} s, {1} MHz busy {2:0.0}%' -f $r.band_seconds, $r.mhz, $r.busy)
    return $r
}

# The same decision from the sampler file. The sampler keeps it open for writing (FileShare.Read), so it is read
# with ReadWrite sharing, and only its tail: a 20-minute session is some 200 KB and the rule needs the last
# minute. An unreadable or absent file is 'no telemetry', never an exception: a measurement window must not be
# lost to a file in use.
function Test-WorldTelemetry {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Path, [datetime]$Now = [DateTime]::Now,
        [int]$TailBytes = 65536, [int]$MenuMHz = 1000, [double]$MinBusy = 90, [int]$MinRunSamples = 8,
        [int]$SettleSeconds = 25, [int]$FreshSeconds = 15)
    $text = ''
    if ($Path) {
        try {
            $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                $start = [Math]::Max(0, $fs.Length - $TailBytes)
                $null = $fs.Seek($start, [IO.SeekOrigin]::Begin)
                $buf = New-Object byte[] ([int][Math]::Min([int64]$TailBytes, $fs.Length - $start))
                $got = $fs.Read($buf, 0, $buf.Length)
                $text = [Text.Encoding]::UTF8.GetString($buf, 0, $got)
            } finally { $fs.Dispose() }
        } catch { $text = '' }
    }
    $samples = if ($text) { Get-WorldSamples -Text $text -Reference $Now } else { @() }
    $r = Test-WorldSignal -Samples $samples -Now $Now -MenuMHz $MenuMHz -MinBusy $MinBusy `
        -MinRunSamples $MinRunSamples -SettleSeconds $SettleSeconds -FreshSeconds $FreshSeconds
    if (!$r.telemetry -and $Path -and !(Test-Path -LiteralPath $Path)) { $r.why = 'no telemetry file' }
    return $r
}
