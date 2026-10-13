$ErrorActionPreference='Stop'
$cli=Join-Path ([string](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot) 'tools\bc250kmd_cli.exe'
$text=& $cli state snapshot 2>&1|Out-String
if($LASTEXITCODE -ne 0){throw "Shared state read failed: $text"}
$state=$text|ConvertFrom-Json
if($state.abi -ne 1 -or $state.error -ne 0){throw 'Invalid shared state snapshot'}
$state.items|Where-Object {$_.path -eq '' -and $_.name -match '^(Cu|Dpm)'}|ConvertTo-Json -Depth 8
