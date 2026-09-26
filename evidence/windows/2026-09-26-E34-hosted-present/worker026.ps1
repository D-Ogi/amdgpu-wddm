$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime014'
$code=125
try {
 & "$dir\run026.ps1" *> "$dir\run026.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run026.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done026.json"
}
exit $code
