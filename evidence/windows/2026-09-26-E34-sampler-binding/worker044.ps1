$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime026'
$code=125
try {
 & "$dir\run044.ps1" *> "$dir\run044.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run044.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done044.json"
}
exit $code
