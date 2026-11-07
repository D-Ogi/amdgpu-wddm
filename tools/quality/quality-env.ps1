# The inputs of a whole quick.ps1 run that do not come from its parameters: the two user-mode driver trees and the
# Visual Studio developer environment. quick.ps1 dot-sources this file and asks it before it plans a gate.
#
# Why. Without BC250_UMD_SOURCE and BC250_UMD_BUILD, three release gates failed with a reason deep inside the run,
# and without a developer shell the gates that compile a fixture with cl.exe failed on a missing compiler. A caller
# that forgot one of them saw failures that looked like defects of the tree, and lane agents reported them as
# "pre-existing" failures (2026-11-01). So a run without the two variables stops before its first gate, with one line
# that names the missing variable, and a run without the developer environment imports it itself.
#   pwsh -File tools\quality\test-quality-env.ps1 -Out <dir>     (the 'quality-env' gate of quick.ps1)

# The one-line reason to refuse the run, or $null. $Values maps a variable name to its value ($null or '' for unset).
function Get-QualityInputRefusal([hashtable]$Values) {
    $names = @('BC250_UMD_SOURCE', 'BC250_UMD_BUILD')
    $unset = @($names | Where-Object { -not $Values[$_] })
    if ($unset.Count) {
        $verb, $it = if ($unset.Count -eq 1) { 'is', 'it' } else { 'are', 'them' }
        return ('{0} {1} not set: set {2} to the umd-gate tree of the release (BC250_UMD_SOURCE: Mesa at the zink-umd row commit, BC250_UMD_BUILD: its configured build)' -f ($unset -join ' and '), $verb, $it)
    }
    foreach ($n in $names) {
        if (-not (Test-Path -LiteralPath $Values[$n] -PathType Container)) { return "$n names a directory that does not exist: $($Values[$n])" }
    }
    return $null
}

# The current values of the two variables, for Get-QualityInputRefusal.
function Get-QualityInputValues {
    $v = @{}
    foreach ($n in @('BC250_UMD_SOURCE', 'BC250_UMD_BUILD')) { $v[$n] = [Environment]::GetEnvironmentVariable($n) }
    return $v
}

# Imports the x64 developer environment into this process when it is not there. Returns what it did, for the run's
# log. common.ps1 is dot-sourced inside this function, so its Set-StrictMode stays in this function's scope.
function Initialize-QualityDevEnvironment([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$TempDir, [string]$Kits = '') {
    . (Join-Path $Repo 'tools\build\common.ps1')
    $install = Find-VsInstall ''
    $kept = Test-VsDevEnvironment $install x64
    $null = Import-VsDevEnvironment -VsInstall $install -TempDir $TempDir -Arch x64
    if ($Kits) {
        # These were formerly expanded by quick.cmd. Keep the chosen WDK first without cmd's line-size limit.
        $front = foreach ($part in 'um', 'shared') { Join-Path $Kits "microsoft.windows.wdk.x64\c\Include\10.0.26100.0\$part" }
        $env:INCLUDE = Get-UniqueEnvironmentPaths ((@($front) + @($env:INCLUDE)) -join ';')
    }
    if ($kept) { return "developer environment: kept ($((Get-Command cl.exe).Source))" }
    return "developer environment: imported x64 from $install ($((Get-Command cl.exe).Source))"
}

# The gates write temporary files. A run whose TEMP is on another drive than the workspace (drive C: of this PC, for
# example) writes them there; quick.cmd moves TEMP to the workspace, and a direct call now does the same.
function Set-QualityTemp([Parameter(Mandatory)][string]$Workspace) {
    $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Workspace))
    if ($env:TEMP -and [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($env:TEMP)) -eq $root) { return "TEMP: kept ($env:TEMP)" }
    $tmp = Join-Path $Workspace 'scratch\tmp'
    [void][IO.Directory]::CreateDirectory($tmp)
    $env:TEMP = $tmp; $env:TMP = $tmp
    return "TEMP: set to $tmp"
}
