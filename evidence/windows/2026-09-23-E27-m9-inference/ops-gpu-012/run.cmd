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
C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3 < NUL > C:\BC250\m9\ops-gpu-012\m8.out 2> C:\BC250\m9\ops-gpu-012\m8.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\m8.exit
if errorlevel 1 exit /b 1
findstr /c:"bc250: progress before submit" C:\BC250\m9\ops-gpu-012\m8.err > NUL
if errorlevel 1 exit /b 2
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\stories15M-1.out 2> C:\BC250\m9\ops-gpu-012\stories15M-1.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\stories15M-1.exit
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\stories15M-2.out 2> C:\BC250\m9\ops-gpu-012\stories15M-2.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\stories15M-2.exit
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\stories15M-3.out 2> C:\BC250\m9\ops-gpu-012\stories15M-3.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\stories15M-3.exit
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\stories15M-4.out 2> C:\BC250\m9\ops-gpu-012\stories15M-4.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\stories15M-4.exit
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\tinyllama-1.out 2> C:\BC250\m9\ops-gpu-012\tinyllama-1.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\tinyllama-1.exit
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\tinyllama-2.out 2> C:\BC250\m9\ops-gpu-012\tinyllama-2.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\tinyllama-2.exit
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\tinyllama-3.out 2> C:\BC250\m9\ops-gpu-012\tinyllama-3.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\tinyllama-3.exit
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-012\tinyllama-4.out 2> C:\BC250\m9\ops-gpu-012\tinyllama-4.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-012\tinyllama-4.exit
if errorlevel 1 exit /b 1
exit /b
