$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime027'
$code=125
try {
 & "$dir\run048.ps1" *> "$dir\run048.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run048.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done048.json"
}
exit $code
