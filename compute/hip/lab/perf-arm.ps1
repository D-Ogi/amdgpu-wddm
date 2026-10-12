# perf-arm.ps1 - one arm of the M16 HIP dispatch-cost plan, under the supervisor of armlib.ps1.
#
# It runs one user-mode program from a staged directory, keeps its output and its own report, and
# bounds itself. It installs nothing and changes no setting.
#
#   python bc250-win\tools\win\target.py ps compute\hip\lab\perf-arm.ps1 -Name p1 `
#       -Exe hipbench.exe -ArgLine "--expect-compute --wait-total 20000 --budget-ms 150000"
#
# The rules the supervisor enforces (and the offline tests of compute\hip\tests\lab prove):
# one deadline of at most 180 s with cleanup inside it, every helper bounded, a refusal when
# there is no trusted temperature, a stop at 89 C at once or at 87 C held for 10 s by the clock,
# a terminated and confirmed child, and a JSON report beside the output.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Exe = 'hipbench.exe',
    [string]$ArgLine = '',
    [string]$Dir = 'C:\BC250\tmp\m16-perf',
    [int]$BoundSec = 170
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'armlib.ps1')

$argv = @()
if ($ArgLine.Trim().Length -gt 0) { $argv = $ArgLine.Trim() -split '\s+' }

$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$reportPath = Join-Path $Dir "$Name-$stamp.report.json"
$report = Invoke-LabArm -Name $Name -Dir $Dir -Exe $Exe -Argv $argv -BoundSec $BoundSec `
    -ReportPath $reportPath
Write-ArmReport -Report $report -ReportPath $reportPath
exit $report.exit_status
