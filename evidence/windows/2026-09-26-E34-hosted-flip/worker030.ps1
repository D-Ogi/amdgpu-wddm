$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime015'
$code=125
try {
 & "$dir\run030.ps1" *> "$dir\run030.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run030.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done030.json"
}
exit $code
