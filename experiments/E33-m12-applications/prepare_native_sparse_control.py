from pathlib import Path
import argparse,hashlib,json
ap=argparse.ArgumentParser(description='Build the native lab probe with actual Mesa sparse batch helpers.')
ap.add_argument('--source',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
a=ap.parse_args();w=a.out.resolve();w.mkdir(parents=True,exist_ok=True)
e=Path(__file__).resolve().parent
bo=a.source/'src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c'
text=bo.read_text()
start=text.index('static VkResult\nradv_wddm2_virtual_bind_begin')
end=text.index('static VkResult\nradv_wddm2_bo_virtual_bind',start)
(w/'radv_sparse_helpers.inc').write_text(text[start:end],newline='\n')
inputs=[bo,e/'native_sparse_control.c',e/'../../tools/win/kmtprobe/kmtprobe.c',e/'../../driver/amdgpu-import/nvd.h']
(w/'inputs.json').write_text(json.dumps([{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in inputs],indent=2)+'\n')
cmd=r"""@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cl /nologo /W3 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS /I "OUT" /I P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um /I P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared /Fo"OUT\native-sparse-control.obj" /Fe"OUT\native-sparse-control.exe" "SOURCE" /link gdi32.lib
exit /b %errorlevel%
"""
cmd=cmd.replace('OUT',str(w)).replace('SOURCE',str(e/'native_sparse_control.c'))
(w/'build.cmd').write_text(cmd,newline='\n')
print('Prepared source-extracted native probe; run only on lab, default bound mode')
