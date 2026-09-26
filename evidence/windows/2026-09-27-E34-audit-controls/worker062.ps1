$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime031'
$code=125
try {
 & "$dir\run062.ps1" *> "$dir\run062.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run062.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done062.json"
}
exit $code
