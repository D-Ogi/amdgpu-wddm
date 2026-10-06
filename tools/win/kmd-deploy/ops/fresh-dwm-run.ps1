# Runs ops/fresh-dwm.ps1 on unit A through bounded-child.exe (Job, whole process tree) with one deadline below the
# three-minute bound (review 829). The helpers come from a retained desktop-umd173 attempt directory, checked by hash
# against the frozen copies; the pushed pre-step script is checked by hash too. A timeout or a nonzero exit is an
# unfinished preparation: witness the lab again, never stage on it.
param([Parameter(Mandatory)][ValidatePattern('^C:\\BC250\\m15\\desktop-umd173-[0-9]{3}$')][string]$Helpers,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ToolSha256,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$InvokeSha256,
 [Parameter(Mandatory)][string]$Script,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ScriptSha256,
 [ValidateRange(60,170)][int]$Seconds=170)
$ErrorActionPreference = 'Stop'
$tool = "$Helpers\bounded-child.exe"
$invoke = "$Helpers\kmd168-transition\invoke-bounded.ps1"
if ((Get-FileHash -LiteralPath $tool).Hash -ne $ToolSha256.ToUpperInvariant()) { throw 'bounded-child.exe hash mismatch' }
if ((Get-FileHash -LiteralPath $invoke).Hash -ne $InvokeSha256.ToUpperInvariant()) { throw 'invoke-bounded.ps1 hash mismatch' }
if ((Get-FileHash -LiteralPath $Script).Hash -ne $ScriptSha256.ToUpperInvariant()) { throw 'fresh-dwm.ps1 hash mismatch' }
. $invoke
$out = Join-Path 'C:\BC250\tmp\fresh-dwm' ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
New-Item -ItemType Directory $out | Out-Null
$deadline = [Diagnostics.Stopwatch]::GetTimestamp() + $Seconds * [Diagnostics.Stopwatch]::Frequency
$receipt = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); seconds = $Seconds; script_sha256 = $ScriptSha256.ToUpperInvariant(); out = $out }
try {
    $r = Invoke-KmdBoundedChild -Tool $tool -Deadline $deadline -Stdout "$out\fresh-dwm.out" -Stderr "$out\fresh-dwm.err" `
        -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile', '-File', $Script)
    $receipt.bounded = $r
} catch { $receipt.error = [string]$_ }
$receipt.done_utc = [DateTime]::UtcNow.ToString('o')
$receipt | ConvertTo-Json -Depth 5 | Set-Content "$out\receipt.json"
$receipt | ConvertTo-Json -Depth 5
if (Test-Path "$out\fresh-dwm.out") { Get-Content "$out\fresh-dwm.out" -Raw }
if (Test-Path "$out\fresh-dwm.err") { Get-Content "$out\fresh-dwm.err" -Raw }
if ($receipt.error -or $receipt.bounded.exit_code -ne 0) { throw 'Fresh-DWM preparation unfinished; witness the lab again' }
