@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=%BC250_ROOT%\toolchain\winflexbison-2.5.25;%PATH%
set INCLUDE=%BC250_ROOT%\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um;%BC250_ROOT%\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared;%INCLUDE%
set PYTHONPATH=%BC250_ROOT%\scratch\py
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%BC250_ROOT%\scratch\tmp
C:\Python314\python.exe -m mesonbuild.mesonmain setup --reconfigure --force-fallback-for=zlib %BC250_ROOT%\scratch\mesa-d3d-build %BC250_ROOT%\scratch\mesa-wddm2 --buildtype=debugoptimized -Dllvm=disabled -Damd-use-llvm=false -Dvulkan-drivers=[] -Dgallium-drivers=softpipe -Dgallium-d3d10umd=true -Dopengl=false -Db_vscrt=mt -Ddefault_library=static -Dplatforms=windows -Dvideo-codecs=[] -Degl=disabled -Dglx=disabled -Dzstd=disabled -Dgallium-d3d10-dll-name=bc250d3d
if errorlevel 1 exit /b 1
C:\msys64\mingw64\bin\ninja.exe -C %BC250_ROOT%\scratch\mesa-d3d-build src/gallium/targets/d3d10umd/bc250d3d.dll
