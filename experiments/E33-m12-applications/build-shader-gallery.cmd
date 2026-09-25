@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
P:\bc-250\scratch\glslang\bin\glslangValidator.exe -V P:\bc-250\bc250-win\experiments\E33-m12-applications\gallery.comp -o P:\bc-250\scratch\m12\shader-gallery\gallery.spv
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /O2 /W3 /MT /I P:\bc-250\toolchain\vvl-deps\Vulkan-Headers\include /FoP:\bc-250\scratch\m12\shader-gallery\shader-gallery-compute.obj /FeP:\bc-250\scratch\m12\shader-gallery\shader-gallery-compute.exe P:\bc-250\bc250-win\experiments\E33-m12-applications\shader-gallery-compute.c P:\bc-250\scratch\vkcompute\vulkan-1.lib
if errorlevel 1 exit /b 1
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /target:winexe /platform:x64 /optimize+ /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll /out:P:\bc-250\scratch\m12\shader-gallery\ShaderGallery.exe P:\bc-250\bc250-win\experiments\E33-m12-applications\ShaderGallery.cs
exit /b %errorlevel%
