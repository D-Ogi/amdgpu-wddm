@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\aperture-state mkdir P:\bc-250\scratch\m9\aperture-state
cd /d P:\bc-250\scratch\m9\aperture-state
cl /nologo /W4 /WX /TC P:\bc-250\bc250-win\experiments\E27-m9-inference\paging-aperture-state-test.c P:\bc-250\bc250-win\driver\kmd\paging_aperture_state.c P:\bc-250\bc250-win\driver\kmd\paging_window.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
