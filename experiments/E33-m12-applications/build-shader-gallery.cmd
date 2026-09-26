@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
%BC250_ROOT%\scratch\glslang\bin\glslangValidator.exe -V %BC250_ROOT%\bc250-win\experiments\E33-m12-applications\gallery.comp -o %BC250_ROOT%\scratch\m12\shader-gallery\gallery.spv
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /O2 /W3 /MT /I %BC250_ROOT%\toolchain\vvl-deps\Vulkan-Headers\include /Fo%BC250_ROOT%\scratch\m12\shader-gallery\shader-gallery-compute.obj /Fe%BC250_ROOT%\scratch\m12\shader-gallery\shader-gallery-compute.exe %BC250_ROOT%\bc250-win\experiments\E33-m12-applications\shader-gallery-compute.c %BC250_ROOT%\scratch\vkcompute\vulkan-1.lib
if errorlevel 1 exit /b 1
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /target:winexe /platform:x64 /optimize+ /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll /out:%BC250_ROOT%\scratch\m12\shader-gallery\ShaderGallery.exe %BC250_ROOT%\bc250-win\experiments\E33-m12-applications\ShaderGallery.cs
exit /b %errorlevel%
