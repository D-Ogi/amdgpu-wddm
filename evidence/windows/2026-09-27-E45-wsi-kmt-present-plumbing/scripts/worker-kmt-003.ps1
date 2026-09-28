$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\wsi-kmt'
$code = 125
try {
  & "$dir\run-kmt-003.ps1" *> "$dir\run-kmt-003.log"
  $code = $LASTEXITCODE
} catch {
  $_ | Out-String | Add-Content "$dir\run-kmt-003.log"
} finally {
  @{ exit = $code; utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content "$dir\done-kmt-003.json"
}
exit $code
