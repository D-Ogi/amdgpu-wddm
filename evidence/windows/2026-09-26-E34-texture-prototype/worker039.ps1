$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime022'
$code=125
try {
 & "$dir\run039.ps1" *> "$dir\run039.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run039.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done039.json"
}
exit $code
