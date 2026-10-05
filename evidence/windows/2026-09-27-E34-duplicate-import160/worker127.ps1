$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime053'
$code=125
try {
 & "$dir\run127.ps1" *> "$dir\run127.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run127.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done127.json"
}
exit $code
