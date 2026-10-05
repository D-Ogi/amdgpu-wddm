$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime019'
$code=125
try {
 & "$dir\run036.ps1" *> "$dir\run036.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run036.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done036.json"
}
exit $code
