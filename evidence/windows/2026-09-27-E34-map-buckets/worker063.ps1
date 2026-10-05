$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime032'
$code=125
try {
 & "$dir\run063.ps1" *> "$dir\run063.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run063.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done063.json"
}
exit $code
