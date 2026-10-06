# The work directory of this client: the built exe and the host run records. It is never inside the
# repository. BC250_FRAMELOOP_WORK names it directly; otherwise it is <BC250_ROOT>\scratch\m15\frameloop,
# and BC250_ROOT defaults to the first directory above this one that holds the toolchain.
function Get-FrameloopWork {
    if ($env:BC250_FRAMELOOP_WORK) { return $env:BC250_FRAMELOOP_WORK }
    $root = $env:BC250_ROOT
    if (-not $root) {
        $probe = $PSScriptRoot
        while ($probe -and -not (Test-Path -LiteralPath (Join-Path $probe 'toolchain'))) { $probe = Split-Path -Parent $probe }
        $root = $probe
    }
    if (-not $root) { throw "workspace root not found from $PSScriptRoot; pass -Exe and -Runs, or set BC250_ROOT" }
    return (Join-Path $root 'scratch\m15\frameloop')
}
