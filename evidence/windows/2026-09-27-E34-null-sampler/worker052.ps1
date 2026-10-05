$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime028'
$code=125
try {
 & "$dir\run052.ps1" *> "$dir\run052.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run052.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done052.json"
}
exit $code
