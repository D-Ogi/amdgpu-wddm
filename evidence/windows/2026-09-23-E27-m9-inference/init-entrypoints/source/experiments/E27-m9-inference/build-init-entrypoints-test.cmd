@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\init-entrypoints mkdir P:\bc-250\scratch\m9\init-entrypoints
cd /d P:\bc-250\scratch\m9\init-entrypoints
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-init-entrypoints-test.py P:\bc-250\bc250-win init.c P:\bc-250\scratch\m9\init-entrypoints-before
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /IP:\bc-250\bc250-win\driver\kmd init.c /Fe:init.exe
if errorlevel 1 exit /b 1
init.exe

if errorlevel 1 exit /b 1
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-init-entrypoints-test.py P:\bc-250\bc250-win mutation.c P:\bc-250\scratch\m9\init-entrypoints-before --omit-completion-check
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /IP:\bc-250\bc250-win\driver\kmd mutation.c /Fe:mutation.exe
if errorlevel 1 exit /b 1
mutation.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
exit /b 0
