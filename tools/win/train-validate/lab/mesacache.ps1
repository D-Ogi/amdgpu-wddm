#Requires -Version 5
# The shader disk cache of the system Vulkan driver, read or moved aside. Read-only in the `read` step, which
# is what the train validation uses: a release whose Vulkan driver is a new build writes a cache of its own,
# because the cache is keyed by the build (BD-100), and the first start of a Vulkan program after the install
# is therefore cold once. Three readings prove the cache is live: it is empty or absent for the new build at
# the start, it grows over the first run, and a warm run adds nothing to it.
#
#   -Step read     files, bytes and the newest write time of each cache directory
#   -Step clear    move a cache aside, so that the next run is cold (renamed, never deleted)
#   -Step restore  put a cache moved aside by `clear` back
#
# The cache lives in the interactive user's profile: a Vulkan game runs in the console session, not in ours.
param(
    [ValidateSet('read', 'clear', 'restore')][string]$Step = 'read',
    [string]$User = 'C:\Users\bc250',
    [string]$Aside = '.trainaside'
)
$ErrorActionPreference = 'Continue'
$dirs = @((Join-Path $User 'AppData\Local\mesa_shader_cache'), (Join-Path $User 'AppData\Local\mesa_shader_cache_db'))
function Report($d) {
    if (Test-Path -LiteralPath $d) {
        $f = @(Get-ChildItem -LiteralPath $d -Recurse -File -Force -ErrorAction SilentlyContinue)
        $sum = [int64]($f | Measure-Object Length -Sum).Sum
        $newest = ($f | Sort-Object LastWriteTimeUtc | Select-Object -Last 1).LastWriteTimeUtc
        if ($newest) { $newest = $newest.ToString('o') } else { $newest = 'none' }
        'cache {0}: {1} files, {2} bytes, newest {3}' -f $d, $f.Count, $sum, $newest
    } else { 'cache {0}: absent' -f $d }
}
"utc $([DateTime]::UtcNow.ToString('o'))"
switch ($Step) {
    'read' { foreach ($d in $dirs) { Report $d } }
    'clear' {
        foreach ($d in $dirs) {
            'before ' + (Report $d)
            if (Test-Path -LiteralPath $d) {
                $target = $d + $Aside
                if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
                Move-Item -LiteralPath $d -Destination $target -Force
                'moved aside -> ' + $target
            }
            'after  ' + (Report $d)
        }
    }
    'restore' {
        foreach ($d in $dirs) {
            $target = $d + $Aside
            if (Test-Path -LiteralPath $target) {
                if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
                Move-Item -LiteralPath $target -Destination $d -Force
                'restored ' + $d
            } else { 'nothing aside for ' + $d }
            Report $d
        }
    }
}
'mesacache step {0} done' -f $Step
