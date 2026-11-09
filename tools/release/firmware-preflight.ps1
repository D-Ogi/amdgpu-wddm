# The firmware preflight line of an installer dry run, read the way the installer means it (BD-325).
# install.ps1 Add-Check 'GPU firmware' writes [ok] when at least one download host answers, and adds
# "; not reachable: <host> <error>" for a host that does not. One host is enough for the install, so a dry-run test
# must not fail a package because the other host did not answer at that second. The hosts are the two of the
# installer's own list; the dry run checks the URLs of both hosts separately.
# Dot-source it (no I/O). Windows PowerShell 5.1 and PowerShell 7.
$script:FirmwareHosts = @('git.kernel.org', 'gitlab.com')

# Returns $null when the text has no [ok] GPU firmware line for a 40-digit linux-firmware commit, else an object:
#   reachable: the hosts of the line that answered (one or both, in the installer's order)
#   unreachable: the text after 'not reachable: ', or '' when both hosts answered
function Get-FirmwarePreflight([string]$Text) {
    $hosts = ($script:FirmwareHosts | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $rx = '\[ok\s*\]\s+GPU firmware\s+download from linux-firmware [0-9a-f]{40}: ((?:' + $hosts + ')(?:, (?:' + $hosts + '))*) reachable(?:; not reachable: ([^\r\n]*))?'
    $m = [regex]::Match($Text, $rx)
    if (-not $m.Success) { return $null }
    $ok = @($m.Groups[1].Value -split ', ')
    if (@($ok | Select-Object -Unique).Count -ne $ok.Count) { return $null }
    [pscustomobject]@{ reachable = $ok; unreachable = $m.Groups[2].Value.Trim() }
}
