$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime020'
$code=125
try {
 & "$dir\run037.ps1" *> "$dir\run037.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run037.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done037.json"
}
exit $code
