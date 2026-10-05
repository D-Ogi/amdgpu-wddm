# Prints the last complete "wddm summary" block of the kernel driver's log ring (every line between the last
# two "wddm summary: node 0" lines), to read the driver's cumulative counters (faults, timeouts, resets).
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
$Cli = Resolve-Cli $Cli
$lines = @(& $Cli log 2>&1 | ForEach-Object { "$_" })
$starts = @(for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match 'wddm summary: node 0 hardware') { $i } })
if ($starts.Count -lt 2) { "summary blocks found: $($starts.Count)"; return }
$a = $starts[$starts.Count - 2]; $b = $starts[$starts.Count - 1]
$lines[$a..($b - 1)] | Where-Object { $_ -notmatch ' gfx: (job|VMID)' }
