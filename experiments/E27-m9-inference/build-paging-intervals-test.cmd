@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\intervals mkdir %BC250_ROOT%\scratch\m9\intervals
cd /d %BC250_ROOT%\scratch\m9\intervals
cl /nologo /W4 /WX /O2 /TC %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\paging-intervals-test.c %BC250_ROOT%\bc250-win\driver\kmd\paging_intervals.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
