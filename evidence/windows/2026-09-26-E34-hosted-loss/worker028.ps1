$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime015'
$code=125
try {
 & "$dir\run028.ps1" *> "$dir\run028.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run028.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done028.json"
}
exit $code
