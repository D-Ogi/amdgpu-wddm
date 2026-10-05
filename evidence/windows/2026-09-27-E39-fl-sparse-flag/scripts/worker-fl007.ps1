$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\fl-probe001'
$code = 125
try {
  & "$dir\run-fl007.ps1" *> "$dir\run-fl007.log"
  $code = $LASTEXITCODE
} catch {
  $_ | Out-String | Add-Content "$dir\run-fl007.log"
} finally {
  @{ exit = $code; utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content "$dir\done-fl007.json"
}
exit $code
