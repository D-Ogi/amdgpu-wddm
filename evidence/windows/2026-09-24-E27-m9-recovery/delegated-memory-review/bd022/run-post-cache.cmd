@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\build\bd022
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-post-cache-test.py P:\bc-250\scratch\m9\display137-build-source actual.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC actual.c /Fe:actual.exe
if errorlevel 1 exit /b 1
actual.exe
if errorlevel 1 exit /b 1
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-post-cache-test.py P:\bc-250\scratch\m9\display137-build-source mutation.c --always-nc
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC mutation.c /Fe:mutation.exe
if errorlevel 1 exit /b 1
mutation.exe > mutation.log
if not errorlevel 1 exit /b 1
findstr /C:"POST cache:" mutation.log
exit /b 0
