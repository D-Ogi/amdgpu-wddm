@echo off
setlocal
set VK_DRIVER_FILES=C:\BC250\m9\radv-main-icd2\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set BC250_TRACE_SUBMITS=0
set PATH=C:\BC250\m8;%PATH%
if "%~1"=="" exit /b 2
%*
exit /b %ERRORLEVEL%
