$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime025'
$code=125
try {
 & "$dir\run043.ps1" *> "$dir\run043.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run043.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done043.json"
}
exit $code
