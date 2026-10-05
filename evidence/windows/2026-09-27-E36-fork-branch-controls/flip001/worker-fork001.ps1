$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\fork-consolidated001'
$code=125
try {
 & "$dir\run-fork001.ps1" *> "$dir\run-fork001.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run-fork001.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done-fork001.json"
}
exit $code
