@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\m9\preemption-test
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-preemption-test.py P:\bc-250\bc250-win\driver\kmd\wddm.c after.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /TC after.c /Fe:after.exe
if errorlevel 1 exit /b 1
after.exe
