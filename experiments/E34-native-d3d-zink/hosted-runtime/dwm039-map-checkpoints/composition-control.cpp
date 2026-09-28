#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string>
static std::wstring directory;
static std::wstring Path(const wchar_t *name){return directory+name;}
static bool animating;
static bool baselineReady;
static HRESULT baselineResult=S_FALSE;
static HWND frontWindow;
static HWND movingWindow;
static ULONGLONG deadline;
static unsigned frames;
static unsigned paints[3];
static bool frozen;
static HRESULT freezeResult=S_FALSE;
static void RecordProgress(){
 LARGE_INTEGER first,last,freq;FILETIME utc;ULARGE_INTEGER ft;
 QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&first);
 GetSystemTimePreciseAsFileTime(&utc);QueryPerformanceCounter(&last);
 ft.LowPart=utc.dwLowDateTime;ft.HighPart=utc.dwHighDateTime;
 RECT moving={};GetClientRect(movingWindow,&moving);
 POINT origin={0,0};ClientToScreen(movingWindow,&origin);
 OffsetRect(&moving,origin.x,origin.y);
 FILE *f=nullptr;
 if(_wfopen_s(&f,Path(L"control-heartbeat.tmp").c_str(),L"w") || !f)return;
 fprintf(f,"{\"baseline_ready\":%s,\"baseline_result\":%ld,\"animating\":%s,\"frames\":%u,\"paint_red\":%u,\"paint_blue\":%u,\"paint_moving\":%u,\"qpc_before\":%lld,\"qpc_after\":%lld,\"qpc_frequency\":%lld,\"utc_filetime\":%llu,\"frozen\":%s,\"freeze_result\":%ld,\"moving_client\":[%ld,%ld,%ld,%ld]}\n",baselineReady?"true":"false",baselineResult,animating?"true":"false",frames,paints[0],paints[1],paints[2],first.QuadPart,last.QuadPart,freq.QuadPart,ft.QuadPart,frozen?"true":"false",freezeResult,moving.left,moving.top,moving.right,moving.bottom);
 fclose(f);
 MoveFileExW(Path(L"control-heartbeat.tmp").c_str(),Path(L"control-heartbeat.json").c_str(),MOVEFILE_REPLACE_EXISTING);
}
static LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM a,LPARAM b){
 if(m==WM_PAINT){++paints[w==frontWindow?1:(w==movingWindow?2:0)];PAINTSTRUCT p;HDC dc=BeginPaint(w,&p);RECT r;GetClientRect(w,&r);HBRUSH brush=CreateSolidBrush(w==frontWindow?RGB(0,0,255):(w==movingWindow?RGB(0,255,255):RGB(255,0,0)));FillRect(dc,&r,brush);DeleteObject(brush);if(w!=frontWindow && w!=movingWindow){RECT marker={8,8,24,24};FillRect(dc,&marker,(HBRUSH)GetStockObject(frames%2?WHITE_BRUSH:BLACK_BRUSH));}EndPaint(w,&p);return 0;}
 // Repaint unchanged contents when an audit marker changes, including frozen
 // captures. This lets an otherwise idle DWM reach its instrumented Flush.
 if(m==WM_TIMER){
  static ULONGLONG markerStamp=0;
  WIN32_FILE_ATTRIBUTE_DATA markerAttr={};
  if(GetFileAttributesExW(Path(L"audit-marker.txt").c_str(),GetFileExInfoStandard,&markerAttr)) {
   ULARGE_INTEGER stamp;stamp.LowPart=markerAttr.ftLastWriteTime.dwLowDateTime;stamp.HighPart=markerAttr.ftLastWriteTime.dwHighDateTime;
   if(stamp.QuadPart!=markerStamp){markerStamp=stamp.QuadPart;InvalidateRect(w,nullptr,FALSE);if(frontWindow)InvalidateRect(frontWindow,nullptr,FALSE);if(movingWindow)InvalidateRect(movingWindow,nullptr,FALSE);}
  }
 }
 if(m==WM_TIMER){if(GetTickCount64()>=deadline){if(movingWindow)DestroyWindow(movingWindow);if(frontWindow)DestroyWindow(frontWindow);DestroyWindow(w);PostQuitMessage(0);}else if(!frozen){
 if(GetFileAttributesW(Path(L"freeze").c_str())!=INVALID_FILE_ATTRIBUTES){
  UpdateWindow(w);UpdateWindow(frontWindow);UpdateWindow(movingWindow);
  freezeResult=DwmFlush();frozen=SUCCEEDED(freezeResult);RecordProgress();return 0;
 }
 if(!animating){
  if(!baselineReady){UpdateWindow(w);UpdateWindow(frontWindow);UpdateWindow(movingWindow);baselineResult=DwmFlush();baselineReady=baselineResult==S_OK && paints[0] && paints[1] && paints[2];}
  RecordProgress();
  if(!baselineReady || GetFileAttributesW(Path(L"animate").c_str())==INVALID_FILE_ATTRIBUTES)return 0;
  animating=true;
 }
 ++frames;if(frames%10==0)RecordProgress();if(frames%5==0){SetWindowPos(movingWindow,HWND_TOPMOST,600+int((frames/5)%20)*8,120+int((frames/5)%10)*6,200+int((frames/5)%5)*16,140+int((frames/5)%7)*8,SWP_NOACTIVATE);InvalidateRect(movingWindow,nullptr,FALSE);}InvalidateRect(w,nullptr,FALSE);InvalidateRect(frontWindow,nullptr,FALSE);}return 0;}
 return DefWindowProcW(w,m,a,b);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int){
 wchar_t executable[MAX_PATH];DWORD length=GetModuleFileNameW(nullptr,executable,MAX_PATH);
 if(!length || length>=MAX_PATH)return 4;
 directory.assign(executable,length);auto slash=directory.find_last_of(L"\\");
 if(slash==std::wstring::npos)return 4;directory.resize(slash+1);
 if(GetFileAttributesW(Path(L"animate").c_str())!=INVALID_FILE_ATTRIBUTES || GetFileAttributesW(Path(L"freeze").c_str())!=INVALID_FILE_ATTRIBUTES || GetFileAttributesW(Path(L"control-heartbeat.json").c_str())!=INVALID_FILE_ATTRIBUTES)return 5;
 SetProcessDPIAware();WNDCLASSW c={};c.lpfnWndProc=WindowProc;c.hInstance=instance;c.lpszClassName=L"BC250G0CompositionControl";if(!RegisterClassW(&c))return 1;
 HWND back=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,c.lpszClassName,L"G0 composition red",WS_POPUP,80,80,320,240,nullptr,nullptr,instance,nullptr);
 frontWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED,c.lpszClassName,L"G0 composition blue",WS_POPUP,160,140,160,120,nullptr,nullptr,instance,nullptr);
 movingWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,c.lpszClassName,L"G0 GPU move and resize control",WS_OVERLAPPEDWINDOW,600,120,200,140,nullptr,nullptr,instance,nullptr);
 if(!back||!frontWindow||!movingWindow)return 2;
 DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_DONOTROUND;
 if(FAILED(DwmSetWindowAttribute(movingWindow,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner))))return 6;
 ShowWindow(movingWindow,SW_SHOWNOACTIVATE);UpdateWindow(movingWindow);
 if(!SetLayeredWindowAttributes(frontWindow,0,128,LWA_ALPHA))return 3;
 ShowWindow(back,SW_SHOWNOACTIVATE);ShowWindow(frontWindow,SW_SHOWNOACTIVATE);UpdateWindow(back);UpdateWindow(frontWindow);
 deadline=GetTickCount64()+270000;SetTimer(back,1,100,nullptr);
 MSG m;while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}return 0;
}
