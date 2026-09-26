@echo off
set VK_DRIVER_FILES=C:\BC250\m9\radv-main-icd2\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
set BC250_TRACE_SUBMITS=0
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3 < NUL > C:\BC250\m9\candidate07130-control\m8.out 2> C:\BC250\m9\candidate07130-control\m8.err
set inference_exit=%ERRORLEVEL%
echo %inference_exit% > C:\BC250\m9\candidate07130-control\m8.exit
if not "%inference_exit%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\candidate07130-control\stories15M.out 2> C:\BC250\m9\candidate07130-control\stories15M.err
set inference_exit=%ERRORLEVEL%
echo %inference_exit% > C:\BC250\m9\candidate07130-control\stories15M.exit
if not "%inference_exit%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\candidate07130-control\tinyllama.out 2> C:\BC250\m9\candidate07130-control\tinyllama.err
set inference_exit=%ERRORLEVEL%
echo %inference_exit% > C:\BC250\m9\candidate07130-control\tinyllama.exit
exit /b %inference_exit%

