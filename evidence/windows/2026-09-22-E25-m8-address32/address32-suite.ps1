$ErrorActionPreference='Stop'
$out='C:\BC250\m8\address32-results'
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item 'C:\BC250\m8\out\compute.txt' "$out\fill-first.txt"
Copy-Item 'C:\BC250\m8\out\compute.err' "$out\fill-first.err"
Copy-Item 'C:\BC250\m8\out\kmd-log.txt' "$out\fill-first-kmd.txt"
$neg='C:\BC250\m8\address32-negative'
New-Item -ItemType Directory -Force $neg | Out-Null
Copy-Item 'C:\BC250\m8\spv\*.spv' $neg
Copy-Item 'C:\BC250\m8\address32\fill.spv' "$neg\fill.spv" -Force
$cmd = "@echo off`r`nset MESA_SHADER_CACHE_DISABLE=true`r`nset BC250_IB_DWORDS=`r`nset PATH=C:\BC250\m8;%PATH%`r`n"
$cmd += "C:\BC250\m8\vulkaninfo.exe --summary > $out\vulkaninfo.txt 2> $out\vulkaninfo.err`r`n"
$cmd += "C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3 > $out\suite.txt 2> $out\suite.err`r`necho suite_exit %ERRORLEVEL%>> $out\suite.txt`r`n"
$cmd += "if errorlevel 2 exit /b 2`r`n"
$cmd += "C:\BC250\m8\vkcompute.exe $neg --only fill_g1 --runs 1 > $out\negative.txt 2> $out\negative.err`r`necho negative_exit %ERRORLEVEL%>> $out\negative.txt`r`n"
$cmd += "C:\BC250\m8\vkcompute.exe $neg --only inthash --runs 1 > $out\positive.txt 2> $out\positive.err`r`necho positive_exit %ERRORLEVEL%>> $out\positive.txt`r`n"
Set-Content 'C:\BC250\m8\address32-suite.cmd' $cmd -Encoding ASCII
$action=New-ScheduledTaskAction -Execute 'C:\BC250\m8\address32-suite.cmd' -WorkingDirectory 'C:\BC250\m8'
$principal=New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'bc250-m8-address32' -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName 'bc250-m8-address32'
Start-Sleep -Seconds 10
Get-ScheduledTask -TaskName 'bc250-m8-address32' | Select-Object State
Get-Content "$out\suite.txt","$out\negative.txt","$out\positive.txt" -ErrorAction SilentlyContinue
