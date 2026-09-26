$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime017'
$code=125
try {
 & "$dir\run032.ps1" *> "$dir\run032.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run032.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done032.json"
}
exit $code
