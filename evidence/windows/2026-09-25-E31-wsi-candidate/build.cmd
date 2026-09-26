@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cl /nologo /std:c11 /W3 /Zi /Od /MD /DVK_USE_PLATFORM_WIN32_KHR /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES /I P:\bc-250\ref\Vulkan-Tools\cube /I P:\bc-250\scratch\mesa-radv-main-20260924\include /FoP:\bc-250\scratch\m10\vkcube\cube.obj /FdP:\bc-250\scratch\m10\vkcube\vkcube.pdb P:\bc-250\scratch\m10\color-oracle\color-oracle.c /FeP:\bc-250\scratch\m10\color-oracle\color-oracle.exe /link user32.lib gdi32.lib shell32.lib dwmapi.lib

