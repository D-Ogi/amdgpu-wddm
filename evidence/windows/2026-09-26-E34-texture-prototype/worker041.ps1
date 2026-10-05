$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime023'
$code=125
try {
 & "$dir\run041.ps1" *> "$dir\run041.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run041.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done041.json"
}
exit $code
