$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime021'
$code=125
try {
 & "$dir\run038.ps1" *> "$dir\run038.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run038.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done038.json"
}
exit $code
