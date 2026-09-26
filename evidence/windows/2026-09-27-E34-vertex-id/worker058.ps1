$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime030'
$code=125
try {
 & "$dir\run058.ps1" *> "$dir\run058.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run058.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done058.json"
}
exit $code
