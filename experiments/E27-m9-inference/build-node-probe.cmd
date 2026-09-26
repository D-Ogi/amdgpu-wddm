@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
copy /y %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\node-probe.cpp %BC250_ROOT%\scratch\m9\llama.cpp-b9564\examples\eval-callback\eval-callback.cpp
if errorlevel 1 exit /b 1
call %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\build-eval-callback.cmd
if errorlevel 1 exit /b 1
copy /y %BC250_ROOT%\scratch\m9\build-cpu-tests\bin\llama-eval-callback.exe %BC250_ROOT%\scratch\m9\build-cpu-tests\bin\bc250-node-probe.exe
