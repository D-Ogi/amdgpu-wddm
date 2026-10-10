# Pure build helpers. Dot-sourcing this file does not change the process environment.
function Get-OrdinalBuildFiles([string]$Pattern) {
    [string[]]$files = @((Get-ChildItem -Path $Pattern -File).FullName)
    [Array]::Sort($files, [StringComparer]::Ordinal)
    return $files
}

function Resolve-Bc250PackageContext([string]$Repo, [string]$Kits, [string]$QualityWorkspace = '') {
    $kitsPath = (Resolve-Path -LiteralPath $Kits -ErrorAction Stop).Path.TrimEnd('\', '/')
    if (-not (Test-Path -LiteralPath $kitsPath -PathType Container)) { throw "Kits is not a directory: $Kits" }
    $selected = $QualityWorkspace
    $origin = 'QualityWorkspace'
    if (-not $selected) { $selected = $env:BC250_ROOT; $origin = 'BC250_ROOT' }
    if (-not $selected) {
        $configured = & git -C $Repo config --local --get bc250.workspace
        if ($LASTEXITCODE -notin @(0, 1)) { throw 'Cannot read repository bc250.workspace configuration' }
        if ($LASTEXITCODE -eq 0 -and $configured) {
            $selected = [string]$configured
            if (-not [IO.Path]::IsPathRooted($selected)) { $selected = Join-Path $Repo $selected }
            $origin = 'git:bc250.workspace'
        }
    }
    if (-not $selected) {
        $toolchain = Split-Path -Parent $kitsPath
        if ((Split-Path -Leaf $kitsPath) -ine 'nuget' -or (Split-Path -Leaf $toolchain) -ine 'toolchain') {
            throw 'Cannot infer quality workspace from Kits; set -QualityWorkspace, BC250_ROOT, or git config --local bc250.workspace'
        }
        $selected = Split-Path -Parent $toolchain
        $origin = 'Kits'
    }
    $workspace = (Resolve-Path -LiteralPath $selected -ErrorAction Stop).Path.TrimEnd('\', '/')
    if (-not (Test-Path -LiteralPath $workspace -PathType Container)) { throw "Quality workspace is not a directory: $selected" }
    $expected = [IO.Path]::GetFullPath((Join-Path $workspace 'toolchain\nuget')).TrimEnd('\', '/')
    if (-not [string]::Equals($expected, $kitsPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Package host suites require Kits at <QualityWorkspace>\toolchain\nuget; refusing inconsistent compiler and quality inputs'
    }
    return [pscustomobject]@{workspace=$workspace; kits=$kitsPath; source=$origin; profile='KmdPackage'}
}
