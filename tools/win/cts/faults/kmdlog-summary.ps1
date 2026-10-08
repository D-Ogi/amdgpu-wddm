# Prints one complete "wddm summary" block of the kernel driver, to read the driver's cumulative counters
# (faults, timeouts, resets).
#
# From KMD 0.7.216.23 a requested summary writes its block beside the log ring (BD-097,
# docs/design/kmd-log-ring.md), so the ring of a running machine holds no block to cut out of it. This script
# therefore asks for the block itself with "log summary only", which costs one Level Two escape and answers the
# whole block. Against an older driver, or a CLI that does not know that form, it falls back to the block
# between the last two "wddm summary: node 0" lines of the ring, which is what it did before.
#
# Each script in this directory resolves the CLI by itself, because target.py ps copies one file to the target.
param([string]$Cli = '')
# The CLI ships with the kernel-driver kit, whose directory changes with every kit (STATE.md names the current
# one): give -Cli, or set BC250_KMD_CLI, or let this take the newest bc250kmd_cli.exe under C:\BC250.
function Resolve-Cli([string]$Path) {
    if ($Path) { return $Path }
    if ($env:BC250_KMD_CLI) { return $env:BC250_KMD_CLI }
    $found = @(Get-ChildItem -LiteralPath 'C:\BC250' -Recurse -Depth 1 -Filter 'bc250kmd_cli.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($found.Count) { return $found[0].FullName }
    throw 'no bc250kmd_cli.exe under C:\BC250; pass -Cli <path>'
}
# The lines of the log, without the tool's own header and footer: a sequence number and a stamp start each one.
function Log-Lines([string[]]$Text) {
    @($Text | Where-Object { $_ -match '^\s*\d+\s+\d+\.\d{3} ' })
}
$Cli = Resolve-Cli $Cli
$lines = Log-Lines @(& $Cli log summary only 2>&1 | ForEach-Object { "$_" })
$starts = @(for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match 'wddm summary: node 0 hardware') { $i } })
if ($starts.Count -ge 1) {
    # The block beside the ring is one summary, so it ends with the answer.
    $lines[$starts[$starts.Count - 1]..($lines.Count - 1)] | Where-Object { $_ -notmatch ' gfx: (job|VMID)' }
    return
}
$lines = Log-Lines @(& $Cli log 2>&1 | ForEach-Object { "$_" })
$starts = @(for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match 'wddm summary: node 0 hardware') { $i } })
if ($starts.Count -lt 2) { "summary blocks found: $($starts.Count)"; return }
$a = $starts[$starts.Count - 2]; $b = $starts[$starts.Count - 1]
$lines[$a..($b - 1)] | Where-Object { $_ -notmatch ' gfx: (job|VMID)' }
