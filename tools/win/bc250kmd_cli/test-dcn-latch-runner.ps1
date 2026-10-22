param([Parameter(Mandatory)][string]$Stub,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {throw 'Elevated runner controls; no adapter is opened'}
$ps51="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$version=& $ps51 -NoProfile -NonInteractive -Command '$PSVersionTable.PSVersion.ToString()'
if ($version -notmatch '^5\.1\.') {throw "Expected Windows PowerShell 5.1, got $version"}
if (Test-Path -LiteralPath $Out) {throw 'Use a new control directory'}
New-Item -ItemType Directory -Path $Out | Out-Null
$Out=(Resolve-Path -LiteralPath $Out).Path
$hash=(Get-FileHash -LiteralPath $Stub).Hash
$results=@()
foreach ($arm in @(0,1)) {
    $capture=Join-Path $Out "exit-$arm"
    $ErrorActionPreference='Continue'
    & $ps51 -NoProfile -NonInteractive -File "$PSScriptRoot\run-dcn-latch.ps1" `
        -Exe $Stub -ExpectedSha256 $hash -ExpectedVersion 0x000700D8 -Arm $arm -Out $capture `
        2>&1 | Out-File "$Out\runner-$arm.txt"
    $runnerExit=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    $record=Get-Content "$capture\run.json" -Raw | ConvertFrom-Json
    $tail=Get-Content "$capture\samples.jsonl" -Raw | ConvertFrom-Json
    if ($null -eq $record.exit_code -or $record.exit_code -ne $arm -or $runnerExit -ne $arm -or
        $tail.exit_code -ne $arm -or !$record.process_exited -or $record.timed_out) {throw "Exit control failed: $capture"}
    $results+=@{expected=$arm; runner_exit=$runnerExit; process_exit=$record.exit_code}
}
@{powershell=$version; stub_sha256=$hash; controls=$results; adapter_opened=$false} |
    ConvertTo-Json -Depth 5 | Set-Content "$Out\result.json" -Encoding UTF8
Write-Output 'Windows PowerShell 5.1 latch runner: exit 0 and exit 1 controls PASS'
