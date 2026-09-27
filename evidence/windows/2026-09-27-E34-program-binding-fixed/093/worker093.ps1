$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime039'
$code=125
try {
 & "$dir\run093.ps1" *> "$dir\run093.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run093.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done093.json"
}
exit $code
