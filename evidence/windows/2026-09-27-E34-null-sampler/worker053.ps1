$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime028'
$code=125
try {
 & "$dir\run053.ps1" *> "$dir\run053.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run053.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done053.json"
}
exit $code
