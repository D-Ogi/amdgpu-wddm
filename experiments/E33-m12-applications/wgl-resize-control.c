#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
typedef HGLRC (WINAPI *CreateContextAttribs)(HDC,HGLRC,const int*);
static void fail(const char *s) { fprintf(stderr,"FAIL %s win32=%lu gl=%x\n",s,GetLastError(),glGetError()); ExitProcess(1); }
static void pump(void) { MSG m; while(PeekMessage(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessage(&m);} }
int main(void) {
 WNDCLASSA wc={0};wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandle(NULL);wc.lpszClassName="BC250WglResize";
 if(!RegisterClassA(&wc))fail("class");
 HWND w=CreateWindowA(wc.lpszClassName,"BC-250 Zink resize/readback control",WS_POPUP,120,120,0,0,0,0,wc.hInstance,0);if(!w)fail("window");
 HDC dc=GetDC(w);PIXELFORMATDESCRIPTOR p={0};p.nSize=sizeof(p);p.nVersion=1;p.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;p.iPixelType=PFD_TYPE_RGBA;p.cColorBits=32;p.cAlphaBits=8;
 int fmt=ChoosePixelFormat(dc,&p);if(!fmt || !SetPixelFormat(dc,fmt,&p))fail("pixel format");
 HGLRC legacy=wglCreateContext(dc);if(!legacy || !wglMakeCurrent(dc,legacy))fail("bootstrap zero");
 CreateContextAttribs create=(CreateContextAttribs)wglGetProcAddress("wglCreateContextAttribsARB");if(!create)fail("create extension");
 int attrs[]={0x2091,4,0x2092,6,0x9126,1,0};HGLRC ctx=create(dc,0,attrs);if(!ctx)fail("core46");
 wglMakeCurrent(0,0);wglDeleteContext(legacy);if(!wglMakeCurrent(dc,ctx))fail("current core46");
 const char *renderer=(const char*)glGetString(GL_RENDERER);printf("renderer=%s version=%s\n",renderer,glGetString(GL_VERSION));if(!renderer || !strstr(renderer,"zink") || !strstr(renderer,"BC-250"))fail("wrong renderer");
 int dims[][2]={{320,240},{0,0},{1,1},{640,360},{0,0},{640,360},{1,1},{0,0},{1,1},{320,240}};
 for(unsigned i=0;i<sizeof(dims)/sizeof(dims[0]);i++){
  int width=dims[i][0],height=dims[i][1];if(!SetWindowPos(w,0,120,120,width,height,SWP_NOZORDER|SWP_NOACTIVATE))fail("resize");ShowWindow(w,SW_SHOWNOACTIVATE);pump();
  RECT rect;if(!GetClientRect(w,&rect) || rect.right!=width || rect.bottom!=height)fail("client size");
  glViewport(0,0,width?width:1,height?height:1);glDisable(GL_DITHER);glDisable(0x8DB9);
  unsigned char expected[4]={(i&1)?255:0,(i&2)?255:0,(i&4)?255:0,255};
  glClearColor(expected[0]/255.0f,expected[1]/255.0f,expected[2]/255.0f,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();if(glGetError()!=GL_NO_ERROR)fail("clear/finish");
  if(width && height){unsigned char pixel[4]={0};glReadBuffer(GL_BACK);glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);if(glGetError()!=GL_NO_ERROR || memcmp(pixel,expected,3))fail("pixel mismatch");}
  if(!SwapBuffers(dc))fail("swap");if(glGetError()!=GL_NO_ERROR)fail("after swap");
  printf("stage=%u size=%dx%d readback=%s swap=OK\n",i,width,height,width?"MATCH":"zero-area skipped");fflush(stdout);Sleep(150);
 }
 wglMakeCurrent(0,0);wglDeleteContext(ctx);ReleaseDC(w,dc);DestroyWindow(w);puts("PASS zero/nonzero same-window readback");return 0;
}
