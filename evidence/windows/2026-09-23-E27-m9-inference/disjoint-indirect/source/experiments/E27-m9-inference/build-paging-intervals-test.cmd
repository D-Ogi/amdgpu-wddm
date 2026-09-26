@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\intervals mkdir P:\bc-250\scratch\m9\intervals
cd /d P:\bc-250\scratch\m9\intervals
cl /nologo /W4 /WX /O2 /TC P:\bc-250\bc250-win\experiments\E27-m9-inference\paging-intervals-test.c P:\bc-250\bc250-win\driver\kmd\paging_intervals.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
