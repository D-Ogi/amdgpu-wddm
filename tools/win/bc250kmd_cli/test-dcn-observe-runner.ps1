param([Parameter(Mandatory)][string]$Stub, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Runner controls require an elevated shell; no adapter is opened.' }
$ps51 = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$version = & $ps51 -NoProfile -NonInteractive -Command '$PSVersionTable.PSVersion.ToString()'
if ($version -notmatch '^5\.1\.') { throw "Expected Windows PowerShell 5.1, got $version" }
$hash = (Get-FileHash -LiteralPath $Stub).Hash
$runRoot = Join-Path $Out ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$results = @()
foreach ($samples in @(1,2)) {
    $capture = Join-Path $runRoot "exit-$($samples-1)"
    # Use the real runner, including its elevation, hash and fresh-output checks.
    # PowerShell 5.1 turns native stderr into error records. Retain the expected
    # nonzero child output without aborting this test before checking run.json.
    $ErrorActionPreference = 'Continue'
    & $ps51 -NoProfile -NonInteractive -File "$PSScriptRoot\run-dcn-observe.ps1" `
        -Exe $Stub -ExpectedSha256 $hash -ExpectedVersion 0x000700D8 `
        -Out $capture -Samples $samples 2>&1 | Out-File "$runRoot\runner-$samples.txt"
    $runnerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $record = Get-Content "$capture\run.json" -Raw | ConvertFrom-Json
    $tail = Get-Content "$capture\samples.jsonl" -Raw | ConvertFrom-Json
    $expected = $samples - 1
    if ($null -eq $record.exit_code -or $record.exit_code -ne $expected -or
        $runnerExit -ne $expected -or $tail.exit_code -ne $expected -or
        !$record.process_exited -or $record.timed_out) {
        throw "Runner exit-code control failed: $capture"
    }
    $results += @{expected=$expected; runner_exit=$runnerExit; process_exit=$record.exit_code; capture=$capture}
}
@{powershell=$version; stub_sha256=$hash; controls=$results; adapter_opened=$false} |
    ConvertTo-Json -Depth 5 | Set-Content "$runRoot\result.json" -Encoding UTF8
Write-Output "Windows PowerShell 5.1 runner: exit 0 and exit 1 controls PASS ($runRoot)"
