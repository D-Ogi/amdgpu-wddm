$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\duplicate-control001'
$code=125
try {
 & "$dir\baseline123.ps1" *> "$dir\run123.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run123.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done123.json"
}
exit $code
