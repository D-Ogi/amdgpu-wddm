@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\oneshot mkdir P:\bc-250\scratch\m9\oneshot
cd /d P:\bc-250\scratch\m9\oneshot
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-oneshot-test.py P:\bc-250\bc250-win test.c %*
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC test.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
