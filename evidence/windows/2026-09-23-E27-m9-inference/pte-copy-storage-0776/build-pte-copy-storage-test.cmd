@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\pte-copy-storage-test mkdir P:\bc-250\scratch\m9\pte-copy-storage-test
cd /d P:\bc-250\scratch\m9\pte-copy-storage-test
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-pte-copy-storage-test.py P:\bc-250\bc250-win\driver\kmd\gfx.c actual.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC actual.c /Fe:actual.exe
if errorlevel 1 exit /b 1
actual.exe
