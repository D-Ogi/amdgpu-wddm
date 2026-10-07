# The template must serve any revision: run test-identity.ps1 in scratch copies whose identity.ps1 names other
# candidate/rollback revisions (as stage.py freeze generates it), and check a wrong directory pattern is refused.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$template = Join-Path (Split-Path -Parent $PSScriptRoot) 'template\kmd-transition'
$fixture = Get-Content -LiteralPath "$template\identity.ps1" -Raw
# A version is R.B (0.7.R.B); the label and the attempt prefix carry R alone for B = 1 and R-B otherwise, as
# stage.py label_suffix writes them. $Label overrides the candidate label, $Abi the candidate ABI revision.
function Get-Suffix([string]$RB) { $p = $RB.Split('.'); if ([int]$p[1] -eq 1) { '{0:D3}' -f [int]$p[0] } else { '{0:D3}-{1}' -f [int]$p[0], [int]$p[1] } }
function Invoke-Variant([string]$Name, [string]$Candidate, [string]$Rollback, [string]$Pattern, [string]$Label = '', [int]$Abi = -1) {
    $dir = Join-Path $Out $Name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item "$template\*.ps1" $dir
    $cr = [int]$Candidate.Split('.')[0]; $rr = [int]$Rollback.Split('.')[0]
    if ($Abi -ge 0) { $cr = $Abi }
    if (!$Label) { $Label = 'candidate' + (Get-Suffix $Candidate) }
    $text = $fixture -replace "KmdCandidateVersion='[^']*'", "KmdCandidateVersion='0.7.$Candidate'" `
        -replace "KmdCandidateAbi='[^']*'", ("KmdCandidateAbi='0x0007{0:X4}'" -f $cr) `
        -replace "KmdCandidateLabel='[^']*'", "KmdCandidateLabel='$Label'" `
        -replace "KmdRollbackVersion='[^']*'", "KmdRollbackVersion='0.7.$Rollback'" `
        -replace "KmdRollbackAbi='[^']*'", ("KmdRollbackAbi='0x0007{0:X4}'" -f $rr) `
        -replace "KmdRollbackLabel='[^']*'", ("KmdRollbackLabel='rollback" + (Get-Suffix $Rollback) + "'") `
        -replace 'kmd[0-9]{3}-deploy\[0-9\]\{3\}', ('kmd' + (Get-Suffix $Pattern) + '-deploy[0-9]{3}')
    [IO.File]::WriteAllText("$dir\identity.ps1", $text)
    $ErrorActionPreference = 'Continue'  # a refused case writes to stderr; that is its expected answer
    $r = & "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$dir\test-identity.ps1" 2>&1 | Out-String
    [IO.File]::WriteAllText("$dir\result.txt", $r)
    return ($LASTEXITCODE -eq 0 -and $r -match 'PASS: identity')
}
$cases = @(
    @{ name = 'r175-over-173'; c = '175.1'; r = '173.1'; p = '175.1'; pass = $true },
    @{ name = 'r200-over-199'; c = '200.1'; r = '199.1'; p = '200.1'; pass = $true },
    @{ name = 'r175-pattern-of-rollback'; c = '175.1'; r = '173.1'; p = '173.1'; pass = $false },
    # Build counters: a candidate build over the release package of the same revision (one ABI), and over a build.
    @{ name = 'r216b16-over-r216b100'; c = '216.16'; r = '216.100'; p = '216.16'; pass = $true },
    @{ name = 'r216b16-over-r216b14'; c = '216.16'; r = '216.14'; p = '216.16'; pass = $true },
    @{ name = 'r217b1-over-r216b100'; c = '217.1'; r = '216.100'; p = '217.1'; pass = $true },
    @{ name = 'r216b16-pattern-of-rollback'; c = '216.16'; r = '216.100'; p = '216.100'; pass = $false },
    @{ name = 'r216b16-pattern-of-build1'; c = '216.16'; r = '216.100'; p = '216.1'; pass = $false },
    @{ name = 'r216b16-same-version'; c = '216.16'; r = '216.16'; p = '216.16'; pass = $false },
    @{ name = 'r216b0'; c = '216.0'; r = '216.100'; p = '216.1'; pass = $false },
    @{ name = 'r216b16-label-without-build'; c = '216.16'; r = '216.100'; p = '216.16'; label = 'candidate216'; pass = $false },
    @{ name = 'r216b16-abi-of-215'; c = '216.16'; r = '216.100'; p = '216.16'; abi = 215; pass = $false })
$failed = @()
foreach ($case in $cases) {
    $label = if ($case.ContainsKey('label')) { $case.label } else { '' }
    $abi = if ($case.ContainsKey('abi')) { $case.abi } else { -1 }
    if ((Invoke-Variant $case.name $case.c $case.r $case.p $label $abi) -ne $case.pass) { $failed += $case.name }
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
'PASS: test-identity holds for 175.1/173.1, 200.1/199.1, 216.16/216.100, 216.16/216.14 and 217.1/216.100; a pattern of the rollback or of build 1, one version twice, build 0, a label without its build and a foreign ABI are refused; router desktop pins pass, three malformed ones are refused'
