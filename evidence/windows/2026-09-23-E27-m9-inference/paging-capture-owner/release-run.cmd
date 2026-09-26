@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\m9\paging-capture
cl /nologo /W4 /WX /O2 /std:c11 P:\bc-250\bc250-win\driver\kmd\paging_capture.c P:\bc-250\scratch\m9\paging-capture\capture-release-test.c /IP:\bc-250\bc250-win\driver\kmd /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
