# The template must serve any revision: run test-identity.ps1 in scratch copies whose identity.ps1 names other
# candidate/rollback revisions (as stage.py freeze generates it), and check a wrong directory pattern is refused.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$template = Join-Path (Split-Path -Parent $PSScriptRoot) 'template\kmd-transition'
$fixture = Get-Content -LiteralPath "$template\identity.ps1" -Raw
function Invoke-Variant([string]$Name, [int]$Candidate, [int]$Rollback, [string]$PatternRevision) {
    $dir = Join-Path $Out $Name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item "$template\*.ps1" $dir
    $text = $fixture -replace "KmdCandidateVersion='[^']*'", "KmdCandidateVersion='0.7.$Candidate.1'" `
        -replace "KmdCandidateAbi='[^']*'", ("KmdCandidateAbi='0x0007{0:X4}'" -f $Candidate) `
        -replace "KmdCandidateLabel='[^']*'", ("KmdCandidateLabel='candidate{0:D3}'" -f $Candidate) `
        -replace "KmdRollbackVersion='[^']*'", "KmdRollbackVersion='0.7.$Rollback.1'" `
        -replace "KmdRollbackAbi='[^']*'", ("KmdRollbackAbi='0x0007{0:X4}'" -f $Rollback) `
        -replace "KmdRollbackLabel='[^']*'", ("KmdRollbackLabel='rollback{0:D3}'" -f $Rollback) `
        -replace 'kmd[0-9]{3}-deploy\[0-9\]\{3\}', ('kmd{0:D3}-deploy[0-9]{{3}}' -f $PatternRevision)
    [IO.File]::WriteAllText("$dir\identity.ps1", $text)
    $ErrorActionPreference = 'Continue'  # a refused case writes to stderr; that is its expected answer
    $r = & "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$dir\test-identity.ps1" 2>&1 | Out-String
    [IO.File]::WriteAllText("$dir\result.txt", $r)
    return ($LASTEXITCODE -eq 0 -and $r -match 'PASS: identity')
}
$cases = @(
    @{ name = 'r175-over-173'; c = 175; r = 173; p = 175; pass = $true },
    @{ name = 'r200-over-199'; c = 200; r = 199; p = 200; pass = $true },
    @{ name = 'r175-pattern-of-rollback'; c = 175; r = 173; p = 173; pass = $false })
$failed = @()
foreach ($case in $cases) {
    if ((Invoke-Variant $case.name $case.c $case.r $case.p) -ne $case.pass) { $failed += $case.name }
}
# The desktop pins as stage.py desktop_pins writes them under the router (switches 1, router + CPU UMD, the router
# key) pass; a router key with the CPU UMD alone, an unsorted set and switches 2 are refused.
$umd = [regex]::Match($fixture, "KmdDesktopUmdSha256='([0-9A-F]{64})'").Groups[1].Value
$router = 'A' * 64
$routerSet = (@($umd, $router) | Sort-Object) -join ','
$desktopCases = @(
    @{ name = 'router-desktop'; s = '1'; m = $routerSet; k = 'SOFTWARE\amdgpu-wddm\DesktopRouter'; pass = $true },
    @{ name = 'router-key-cpu-only'; s = '1'; m = $umd; k = 'SOFTWARE\amdgpu-wddm\DesktopRouter'; pass = $false },
    @{ name = 'router-set-unsorted'; s = '1'; m = ((@($umd, $router) | Sort-Object -Descending) -join ','); k = 'SOFTWARE\amdgpu-wddm\DesktopRouter'; pass = $false },
    @{ name = 'switches-2'; s = '2'; m = $umd; k = ''; pass = $false })
foreach ($case in $desktopCases) {
    $dir = Join-Path $Out $case.name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item "$template\*.ps1" $dir
    $text = $fixture -replace "KmdDesktopSwitches='[^']*'", ("KmdDesktopSwitches='" + $case.s + "'") `
        -replace "KmdDesktopModules='[^']*'", ("KmdDesktopModules='" + $case.m + "'") `
        -replace "KmdDesktopRouterKey='[^']*'", ("KmdDesktopRouterKey='" + $case.k + "'")   # .NET replacement: only $ is special
    [IO.File]::WriteAllText("$dir\identity.ps1", $text)
    $ErrorActionPreference = 'Continue'
    $r = & "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$dir\test-identity.ps1" 2>&1 | Out-String
    [IO.File]::WriteAllText("$dir\result.txt", $r)
    if ((($LASTEXITCODE -eq 0) -and ($r -match 'PASS: identity')) -ne $case.pass) { $failed += $case.name }
}
if ($failed.Count) { throw "identity revision cases failed: $($failed -join ', ')" }
'PASS: test-identity holds for revisions 175/173 and 200/199; a pattern naming the rollback revision is refused; router desktop pins pass, three malformed ones are refused'
