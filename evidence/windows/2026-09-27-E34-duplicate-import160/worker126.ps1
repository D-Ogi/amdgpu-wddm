$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\duplicate-control002'
$code=125
try {
 & "$dir\baseline126.ps1" *> "$dir\run126.log"
 $code=$LASTEXITCODE
} catch {
 $_ | Out-String | Add-Content "$dir\run126.log"
} finally {
 @{exit=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done126.json"
}
exit $code
