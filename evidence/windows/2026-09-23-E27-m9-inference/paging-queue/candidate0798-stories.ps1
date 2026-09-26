$ErrorActionPreference='Stop'
$env:VK_DRIVER_FILES='C:\BC250\m9\quiet-submit\radeon_icd.json'
$env:VK_ICD_FILENAMES=$env:VK_DRIVER_FILES
$env:MESA_SHADER_CACHE_DISABLE='true'
$env:BC250_TRACE_SUBMITS='0'
$env:PATH='C:\BC250\m8;'+$env:PATH
& C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p 'Once upon a time' -n 32 -ngl 99 -c 512 -t 4 --seed 1 --temp 0
'inference_exit='+$LASTEXITCODE
& C:\BC250\m8\bc250kmd_cli.exe log summary
'inference_observation_end'
