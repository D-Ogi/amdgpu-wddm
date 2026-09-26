@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\stop-chain mkdir P:\bc-250\scratch\m9\stop-chain
cd /d P:\bc-250\scratch\m9\stop-chain
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-stop-chain-test.py P:\bc-250\scratch\m9\stop-chain-before before.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC before.c /Fe:before.exe
if errorlevel 1 exit /b 1
before.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-stop-chain-test.py P:\bc-250\bc250-win after.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC after.c /Fe:after.exe
if errorlevel 1 exit /b 1
after.exe
