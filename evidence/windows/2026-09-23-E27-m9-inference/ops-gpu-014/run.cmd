@echo off
set VK_DRIVER_FILES=C:\BC250\m9\gather-fence\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set MESA_SHADER_CACHE_DISABLE=true
set VK_LOADER_DEBUG=driver
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
set GGML_VK_DISABLE_F16=
set GGML_VK_DISABLE_GRAPH_OPTIMIZE=
set GGML_VK_DISABLE_FUSION=
set GGML_VK_DISABLE_ASYNC=
set GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM=
C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3 < NUL > C:\BC250\m9\ops-gpu-014\m8.out 2> C:\BC250\m9\ops-gpu-014\m8.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-014\m8.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
findstr /c:"bc250: progress before submit" C:\BC250\m9\ops-gpu-014\m8.err > NUL
if errorlevel 1 exit /b 2
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\ops-gpu-014\stories15M.out 2> C:\BC250\m9\ops-gpu-014\stories15M.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-014\stories15M.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\ops-gpu-014\tinyllama.out 2> C:\BC250\m9\ops-gpu-014\tinyllama.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-014\tinyllama.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
exit /b
