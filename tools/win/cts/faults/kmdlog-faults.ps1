# Prints the kernel driver's log ring lines that name a VM/UTCL2 fault, a fence timeout or a reset, plus the
# ring's last lines and the first lines of the dpm report, so a CTS batch can be checked for kernel-side faults
# afterwards. Each script in this directory resolves the CLI by itself, because target.py ps copies one file.
param([string]$Cli = '', [int]$Tail = 6)
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
$utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fff')
"kmdlog-faults at ${utc}Z, cli $Cli"
$lines = @(& $Cli log 2>&1 | ForEach-Object { "$_" })
"ring lines: $($lines.Count)"
$hits = @($lines | Where-Object { $_ -match '(?i)fault|utcl2|vm_?l2|protection|timeout|reset|hang|tdr|memviol' })
"fault-like lines: $($hits.Count)"
$hits | Select-Object -Last 40
'--- tail'
$lines | Select-Object -Last $Tail
& $Cli dpm 2>&1 | Select-Object -First 3
