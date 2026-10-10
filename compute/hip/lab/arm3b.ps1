# arm3b.ps1 - one arm of the M16 step-3B plan or of a dispatch-cost re-run set, under the
# supervisor of armlib.ps1. It runs one staged user-mode program with an environment of its own,
# keeps its output, its redirect and its report, and bounds itself. It installs nothing and
# changes no setting: the environment variables go to the child and are removed on every path.
#
#   python bc250-win\tools\win\target.py ps compute\hip\lab\arm3b.ps1 `
#       -Name a-gemm -Dir C:\BC250\tmp\m16-hip3b -Exe gemm_check.exe -ArgLine "--wait-total 20000"
#
# -EnvPairs 'BC250_HIP_BATCH=0,BC250_HIP_BARRIER=full'  environment for the child only. A comma
#                                                       and a semicolon both separate; prefer the
#                                                       comma, because the shell that starts this
#                                                       script over ssh reads a semicolon as a
#                                                       statement separator unless the whole value
#                                                       is quoted.
# -Redirect hip-64.txt                                  keep stdout under that name as well
# -PromptText '...'                                     one argument that holds spaces, appended
#                                                       after -p
# -BoundSec 170                                         the arm's own bound, at most 180 s
#
# The rules the supervisor enforces, and the offline tests of compute\hip\tests\lab prove without
# a GPU: one deadline with cleanup inside it, every helper bounded, a refusal when there is no
# trusted temperature, a stop at 89 C at once or at 87 C held for 10 s of elapsed time, a child
# terminated as a tree and confirmed, an honest UNKNOWN when termination cannot be confirmed, and
# a JSON report beside the output.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Dir,
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$ArgLine = '',
    [string]$EnvPairs = '',
    [string]$Redirect = '',
    [string]$PromptText = '',
    [int]$BoundSec = 170
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'armlib.ps1')

$childEnv = @{}
if ($EnvPairs.Trim().Length -gt 0) {
    foreach ($pair in ($EnvPairs -split '[;,]')) {
        if ($pair.Trim().Length -eq 0) { continue }
        $kv = $pair.Split('=', 2)
        if ($kv.Count -ne 2 -or $kv[0].Trim().Length -eq 0) {
            "arm $Name : -EnvPairs holds '$pair', which is not name=value"
            exit 2
        }
        $childEnv[$kv[0].Trim()] = $kv[1]
    }
}

$argv = @()
if ($ArgLine.Trim().Length -gt 0) { $argv += ($ArgLine.Trim() -split '\s+') }
# -PromptText carries one argument that holds spaces, which -ArgLine cannot: it is split on
# whitespace. It is appended after -p, so the program sees the prompt as one word of argv.
if ($PromptText.Length -gt 0) { $argv += '-p'; $argv += ('"' + $PromptText + '"') }

$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$reportPath = Join-Path $Dir "$Name-$stamp.report.json"
$report = Invoke-LabArm -Name $Name -Dir $Dir -Exe $Exe -Argv $argv -ChildEnv $childEnv `
    -Redirect $Redirect -BoundSec $BoundSec -ReportPath $reportPath
Write-ArmReport -Report $report -ReportPath $reportPath
exit $report.exit_status
