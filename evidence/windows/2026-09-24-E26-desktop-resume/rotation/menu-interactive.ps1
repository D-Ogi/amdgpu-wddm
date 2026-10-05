$ErrorActionPreference='Stop'
$sh=New-Object -ComObject WScript.Shell
$sh.SendKeys('^{ESC}')
Start-Sleep -Seconds 12
$sh.SendKeys('{ESC}')
'menu_keys_complete' | Set-Content C:\BC250\m13\menu-control\done.txt
