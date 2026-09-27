$ErrorActionPreference='Stop'
$out='C:\BC250\m13\kmt-flip001'
$code=125
try {
 & "$out\run-kmtflip001.ps1" *> "$out\run-kmtflip001.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$out\run-kmtflip001.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$out\done-kmtflip001.json"
}
exit $code
