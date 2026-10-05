@echo off
set VK_DRIVER_FILES=C:\BC250\m9\quiet-submit\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
set BC250_TRACE_SUBMITS=0
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\stories15M-1.out 2> C:\BC250\m9\repeat0798\stories15M-1.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\stories15M-1.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\tinyllama-1.out 2> C:\BC250\m9\repeat0798\tinyllama-1.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\tinyllama-1.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\stories15M-2.out 2> C:\BC250\m9\repeat0798\stories15M-2.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\stories15M-2.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\tinyllama-2.out 2> C:\BC250\m9\repeat0798\tinyllama-2.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\tinyllama-2.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\stories15M-3.out 2> C:\BC250\m9\repeat0798\stories15M-3.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\stories15M-3.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\tinyllama-3.out 2> C:\BC250\m9\repeat0798\tinyllama-3.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\tinyllama-3.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\stories15M-4.out 2> C:\BC250\m9\repeat0798\stories15M-4.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\stories15M-4.exit
if not "%result%"=="0" exit /b %result%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\repeat0798\tinyllama-4.out 2> C:\BC250\m9\repeat0798\tinyllama-4.err
set result=%ERRORLEVEL%
echo %result% > C:\BC250\m9\repeat0798\tinyllama-4.exit
if not "%result%"=="0" exit /b %result%
exit /b 0

