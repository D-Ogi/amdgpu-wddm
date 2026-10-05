#include <windows.h>
static HWND frontWindow;
static ULONGLONG deadline;
static unsigned frames;
static LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM a,LPARAM b){
 if(m==WM_PAINT){PAINTSTRUCT p;HDC dc=BeginPaint(w,&p);RECT r;GetClientRect(w,&r);HBRUSH brush=CreateSolidBrush(w==frontWindow?RGB(0,0,255):RGB(255,0,0));FillRect(dc,&r,brush);DeleteObject(brush);if(w!=frontWindow){RECT marker={8,8,24,24};FillRect(dc,&marker,(HBRUSH)GetStockObject(frames%2?WHITE_BRUSH:BLACK_BRUSH));}EndPaint(w,&p);return 0;}
 if(m==WM_TIMER){if(GetTickCount64()>=deadline){if(frontWindow)DestroyWindow(frontWindow);DestroyWindow(w);PostQuitMessage(0);}else{++frames;InvalidateRect(w,nullptr,FALSE);InvalidateRect(frontWindow,nullptr,FALSE);}return 0;}
 return DefWindowProcW(w,m,a,b);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int){
 SetProcessDPIAware();WNDCLASSW c={};c.lpfnWndProc=WindowProc;c.hInstance=instance;c.lpszClassName=L"BC250G0CompositionControl";if(!RegisterClassW(&c))return 1;
 HWND back=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,c.lpszClassName,L"G0 composition red",WS_POPUP,80,80,320,240,nullptr,nullptr,instance,nullptr);
 frontWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED,c.lpszClassName,L"G0 composition blue",WS_POPUP,160,140,160,120,nullptr,nullptr,instance,nullptr);
 if(!back||!frontWindow)return 2;
 if(!SetLayeredWindowAttributes(frontWindow,0,128,LWA_ALPHA))return 3;
 ShowWindow(back,SW_SHOWNOACTIVATE);ShowWindow(frontWindow,SW_SHOWNOACTIVATE);UpdateWindow(back);UpdateWindow(frontWindow);
 deadline=GetTickCount64()+75000;SetTimer(back,1,100,nullptr);
 MSG m;while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}return 0;
}
