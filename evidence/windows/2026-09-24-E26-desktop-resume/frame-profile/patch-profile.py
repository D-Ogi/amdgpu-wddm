from pathlib import Path
import difflib
r=Path('scratch/mesa-wddm2/src/gallium/frontends/d3d10umd');b=Path('scratch/m13/profile-before');b.mkdir()
for n in ['State.h','Draw.cpp','DxgiFns.cpp','Debug.cpp']:(b/n).write_bytes((r/n).read_bytes())
p=r/'State.h';t=p.read_text().replace('   RenderTargetView *renderTargetViews;','   UINT64 profileDrawTicks, profileDrawMax, profileDrawCalls, profileLastPresent;\n   UINT profilePresents;\n   RenderTargetView *renderTargetViews;');p.write_text(t,newline='\n')
p=r/'Draw.cpp';t=p.read_text();pos=t.index('static unsigned\nClampedUAdd');t=t[:pos]+'''// Diagnostic wall-clock accounting around native draw entry points.
struct Bc250DrawTimer {
   Device *device; LARGE_INTEGER start;
   Bc250DrawTimer(Device *d):device(d){QueryPerformanceCounter(&start);}
   ~Bc250DrawTimer(){LARGE_INTEGER end;QueryPerformanceCounter(&end);
      UINT64 ticks=end.QuadPart-start.QuadPart;
      device->profileDrawTicks+=ticks;device->profileDrawCalls++;
      if(ticks>device->profileDrawMax)device->profileDrawMax=ticks;}
};

'''+t[pos:];t=t.replace('   Device *pDevice = CastDevice(hDevice);','   Device *pDevice = CastDevice(hDevice);\n   Bc250DrawTimer timer(pDevice);');p.write_text(t,newline='\n')
p=r/'DxgiFns.cpp';t=p.read_text();a='   hr = device->pDXGIBaseCallbacks->pfnPresentCb(device->hDevice, &present);';new='''   LARGE_INTEGER presentStart, presentEnd, frequency;
   QueryPerformanceCounter(&presentStart);
'''+a+'''
   QueryPerformanceCounter(&presentEnd);QueryPerformanceFrequency(&frequency);
   if(++device->profilePresents<=120 || device->profilePresents%60==0)
      DebugPrintf("BC250 Perf frame %u gap_ms %.3f draws %llu draw_ms %.3f max_draw_ms %.3f present_ms %.3f\\n",
       device->profilePresents,
       device->profileLastPresent?1000.0*(presentEnd.QuadPart-device->profileLastPresent)/frequency.QuadPart:0.0,
       device->profileDrawCalls,1000.0*device->profileDrawTicks/frequency.QuadPart,
       1000.0*device->profileDrawMax/frequency.QuadPart,
       1000.0*(presentEnd.QuadPart-presentStart.QuadPart)/frequency.QuadPart);
   device->profileLastPresent=presentEnd.QuadPart;
   device->profileDrawTicks=device->profileDrawMax=device->profileDrawCalls=0;
''';assert t.count(a)==1;t=t.replace(a,new);p.write_text(t,newline='\n')
p=r/'Debug.cpp';t=p.read_text().replace('|| strstr(buf, "BC250 SetError")','|| strstr(buf, "BC250 SetError") || strstr(buf, "BC250 Perf")');p.write_text(t,newline='\n')
patch=''.join(''.join(difflib.unified_diff((b/n).read_text().splitlines(True),(r/n).read_text().splitlines(True),fromfile='a/src/gallium/frontends/d3d10umd/'+n,tofile='b/src/gallium/frontends/d3d10umd/'+n)) for n in ['State.h','Draw.cpp','DxgiFns.cpp','Debug.cpp'])
Path('bc250-win/experiments/E26-wddm-desktop/mesa-frame-profile.patch').write_text(patch,newline='\n')
