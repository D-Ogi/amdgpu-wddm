@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\m9\shared-cpu-cache
cl /nologo /TC /W4 /WX /wd4201 /wd4214 /D_AMD64_ /DAMD64 /I P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\km /I P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared /I P:\bc-250\bc250-win\driver\kmd %1 P:\bc-250\bc250-win\driver\kmd\umd_blob.c /Fe:%2
if errorlevel 1 exit /b 1
%2
