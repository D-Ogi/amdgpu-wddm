@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\aperture-state mkdir %BC250_ROOT%\scratch\m9\aperture-state
cd /d %BC250_ROOT%\scratch\m9\aperture-state
cl /nologo /W4 /WX /TC %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\paging-aperture-state-test.c %BC250_ROOT%\bc250-win\driver\kmd\paging_aperture_state.c %BC250_ROOT%\bc250-win\driver\kmd\paging_window.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
