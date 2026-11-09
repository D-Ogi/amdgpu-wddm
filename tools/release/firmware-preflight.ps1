# The firmware preflight line of an installer dry run, read the way the installer means it (BD-325).
# install.ps1 Add-Check 'GPU firmware' writes [ok] when at least one download host answers, and adds
# "; not reachable: <host> <error>" for a host that does not. One host is enough for the install, so a dry-run test
# must not fail a package because the other host did not answer at that second. The dry run checks the URLs of both
# hosts separately.
# The hosts come from the firmware url_templates of the manifest, the list that the installer probes
# (common.ps1 Test-FirmwareHosts through Get-FirmwareUrls), so this file keeps no copy of it.
# Dot-source it (no I/O). Windows PowerShell 5.1 and PowerShell 7.

# The download hosts of a manifest's "firmware" object, in the installer's order.
function Get-FirmwareHosts($Firmware) {
    $hosts = @()
    foreach ($t in @($Firmware.url_templates)) {
        if ([string]$t -match '^https?://([^/:?]+)' -and $hosts -notcontains $Matches[1]) { $hosts += $Matches[1] }
    }
    return , $hosts
}

# Returns $null when the text has no [ok] GPU firmware line for a 40-digit linux-firmware commit that names only
# $Hosts, else an object:
#   reachable: the hosts of the line that answered (one or both, in the installer's order)
#   unreachable: the text after 'not reachable: ', or '' when both hosts answered
function Get-FirmwarePreflight([string]$Text, [string[]]$Hosts) {
    if (-not @($Hosts).Count) { return $null }
    $names = (@($Hosts) | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $rx = '\[ok\s*\]\s+GPU firmware\s+download from linux-firmware [0-9a-f]{40}: ((?:' + $names + ')(?:, (?:' + $names + '))*) reachable(?:; not reachable: ([^\r\n]*))?'
    $m = [regex]::Match($Text, $rx)
    if (-not $m.Success) { return $null }
    $ok = @($m.Groups[1].Value -split ', ')
    if (@($ok | Select-Object -Unique).Count -ne $ok.Count) { return $null }
    [pscustomobject]@{ reachable = $ok; unreachable = $m.Groups[2].Value.Trim() }
}

# The dry run's warning line for a host that did not answer, or $null when every host answered (or no line).
function Get-FirmwarePreflightWarning($Preflight) {
    if ($Preflight -and $Preflight.unreachable) {
        return "  WARN preflight: firmware host not reachable from this computer now: $($Preflight.unreachable)"
    }
    return $null
}
