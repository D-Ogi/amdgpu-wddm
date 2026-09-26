$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime029'
$code=125
try {
 & "$dir\run056.ps1" *> "$dir\run056.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run056.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done056.json"
}
exit $code
