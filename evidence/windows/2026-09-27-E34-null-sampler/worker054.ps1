$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime028'
$code=125
try {
 & "$dir\run054.ps1" *> "$dir\run054.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run054.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done054.json"
}
exit $code
