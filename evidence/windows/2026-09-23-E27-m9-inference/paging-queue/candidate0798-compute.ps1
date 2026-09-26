$ErrorActionPreference='Stop'
$env:VK_DRIVER_FILES='C:\BC250\m9\quiet-submit\radeon_icd.json'
$env:VK_ICD_FILENAMES=$env:VK_DRIVER_FILES
$env:MESA_SHADER_CACHE_DISABLE='true'
$env:BC250_TRACE_SUBMITS='0'
$env:PATH='C:\BC250\m8;'+$env:PATH
'icd_manifest'
Get-Content $env:VK_DRIVER_FILES
'compute_begin='+(Get-Date).ToString('s')
& C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3
'compute_exit='+$LASTEXITCODE
& C:\BC250\m8\bc250kmd_cli.exe log summary
'compute_observation_end'
