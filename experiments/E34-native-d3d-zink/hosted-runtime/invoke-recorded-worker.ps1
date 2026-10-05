param(
 [Parameter(Mandatory)][string]$Worker,
 [Parameter(Mandatory)][string]$ReceiptDirectory,
 [Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$Attempt)
$ErrorActionPreference='Stop'
function Write-NewReceipt([string]$Path,$Value) {
 $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 8))
 $stream=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
 try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)} finally {$stream.Dispose()}
}
# Root exists before the task is launched. Receipts precede any worker preflight.
$root=(Resolve-Path -LiteralPath $ReceiptDirectory).Path
$started=Join-Path $root "$Attempt-started.json"
$terminal=Join-Path $root "$Attempt-terminal.json"
$log=Join-Path $root "$Attempt-output.txt"
if((Test-Path $terminal) -or (Test-Path $log)){throw 'Existing worker attempt; inspect original'}
Write-NewReceipt $started @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;start=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o');worker=$Worker}
$code=1;$failure=$null
try {
 $global:LASTEXITCODE=0
 & $Worker *>&1 | Out-File -LiteralPath $log -Encoding utf8
 $code=$LASTEXITCODE
} catch {
 $failure=@{message=$_.Exception.Message;error_id=$_.FullyQualifiedErrorId;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace}
 $_ | Out-String | Out-File -LiteralPath $log -Append -Encoding utf8
 $code=1
} finally {
 if(Test-Path $log){
  $stream=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
  try {$stream.Flush($true)} finally {$stream.Dispose()}
 }
 Write-NewReceipt $terminal @{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;failure=$failure;pid=$PID}
}
exit $code
