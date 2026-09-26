$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime018'
$code=125
try {
 & "$dir\run034.ps1" *> "$dir\run034.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run034.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done034.json"
}
exit $code
