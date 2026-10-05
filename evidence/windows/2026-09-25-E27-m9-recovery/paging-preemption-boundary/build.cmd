@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\m9\paging-preemption-boundary
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-paging-preemption-test.py P:\bc-250\bc250-win test.c %*
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /I P:\bc-250\bc250-win\driver\kmd test.c P:\bc-250\bc250-win\driver\kmd\paging_private.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
