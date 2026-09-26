@echo off
set VK_DRIVER_FILES=C:\BC250\m9\no-icd-for-cpu-control.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories260K.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 0 -no-cnv -t 6 < NUL > C:\BC250\m9\cpu-stories260K.out 2> C:\BC250\m9\cpu-stories260K.err
echo exit %ERRORLEVEL%
if errorlevel 1 exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 0 -no-cnv -t 6 < NUL > C:\BC250\m9\cpu-stories15M.out 2> C:\BC250\m9\cpu-stories15M.err
echo exit %ERRORLEVEL%
if errorlevel 1 exit /b 1
