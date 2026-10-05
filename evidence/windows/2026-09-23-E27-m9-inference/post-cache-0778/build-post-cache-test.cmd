@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\post-cache-test mkdir P:\bc-250\scratch\m9\post-cache-test
cd /d P:\bc-250\scratch\m9\post-cache-test
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-post-cache-test.py P:\bc-250\bc250-win mutation.c --always-nc
cl /nologo /W4 /WX /TC mutation.c /Fe:mutation.exe
if errorlevel 1 exit /b 1
mutation.exe
if not errorlevel 1 exit /b 1
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-post-cache-test.py P:\bc-250\bc250-win actual.c
cl /nologo /W4 /WX /TC actual.c /Fe:actual.exe
if errorlevel 1 exit /b 1
actual.exe
