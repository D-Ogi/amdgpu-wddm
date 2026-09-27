$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\witcher3-dx12'
$code = 125
try {
  & "$dir\run-w3dx12-002.ps1" *> "$dir\run-w3dx12-002.log"
  $code = $LASTEXITCODE
} catch {
  $_ | Out-String | Add-Content "$dir\run-w3dx12-002.log"
} finally {
  @{ exit = $code; utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content "$dir\done-w3dx12-002.json"
}
exit $code
