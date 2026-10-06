@echo off
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\llama-b19-20261006T183310Z\stories15M.out 2> C:\BC250\m9\llama-b19-20261006T183310Z\stories15M.err
echo %ERRORLEVEL% > C:\BC250\m9\llama-b19-20261006T183310Z\stories15M.exit
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\llama-b19-20261006T183310Z\tinyllama.out 2> C:\BC250\m9\llama-b19-20261006T183310Z\tinyllama.err
echo %ERRORLEVEL% > C:\BC250\m9\llama-b19-20261006T183310Z\tinyllama.exit
