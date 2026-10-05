# One kernel driver summary read: object and allocation open/close counters (bc250kmd_cli log summary), one
# line each, so a leak over a CTS batch shows up. Run it before and after the batch with a -Tag each time.
# Each script in this directory resolves the CLI by itself, because target.py ps copies one file to the target.
param([string]$Cli = '', [string]$Tag = '')
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
$out = & $Cli log summary 2>&1
$t = (Get-Date).ToUniversalTime().ToString('HH:mm:ss')
$out | Where-Object { $_ -match 'objects created/destroyed|allocations opened/closed' } | ForEach-Object { "$t $Tag $_" }
