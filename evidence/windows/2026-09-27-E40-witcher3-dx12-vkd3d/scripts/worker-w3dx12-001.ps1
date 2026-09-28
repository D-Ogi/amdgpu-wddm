$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\witcher3-dx12'
$code = 125
try {
  & "$dir\run-w3dx12-001.ps1" *> "$dir\run-w3dx12-001.log"
  $code = $LASTEXITCODE
} catch {
  $_ | Out-String | Add-Content "$dir\run-w3dx12-001.log"
} finally {
  @{ exit = $code; utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content "$dir\done-w3dx12-001.json"
}
exit $code
