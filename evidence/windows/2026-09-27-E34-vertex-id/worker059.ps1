$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime030'
$code=125
try {
 & "$dir\run059.ps1" *> "$dir\run059.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run059.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done059.json"
}
exit $code
