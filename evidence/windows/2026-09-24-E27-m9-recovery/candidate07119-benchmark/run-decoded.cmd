@echo off
set VK_DRIVER_FILES=C:\BC250\m9\cache-intent-v2\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
set BC250_TRACE_SUBMITS=0
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\bench07119\stories15M.out 2> C:\BC250\m9\bench07119\stories15M.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\bench07119\stories15M.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\bench07119\tinyllama.out 2> C:\BC250\m9\bench07119\tinyllama.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\bench07119\tinyllama.exit
if not "%result%"=="0" exit /b %result%
exit /b 0
