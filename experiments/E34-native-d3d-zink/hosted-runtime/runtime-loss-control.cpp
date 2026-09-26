#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
using Microsoft::WRL::ComPtr;
static LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(){
 bool expectLoss=GetEnvironmentVariableA("BC250_HOST_TEST_LOSS",nullptr,0)!=0;
 setvbuf(stdout,nullptr,_IONBF,0);
 SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1");
 ComPtr<IDXGIFactory1> f;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))return 1;
 ComPtr<IDXGIAdapter1> adapter;
 for(UINT n=0;;n++){
  ComPtr<IDXGIAdapter1>a;if(f->EnumAdapters1(n,&a)==DXGI_ERROR_NOT_FOUND)break;
  DXGI_ADAPTER_DESC1 d={};a->GetDesc1(&d);
  if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=a;unsigned long long luid=0;memcpy(&luid,&d.AdapterLuid,8);char t[32];sprintf_s(t,"%016llx",luid);SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",t);break;}
 }
 if(!adapter)return 2;
 WNDCLASSW wc={};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"BC250HostedPresent";
 if(!RegisterClassW(&wc))return 3;
 RECT rect={0,0,320,240};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
 HWND w=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"G0 hosted GPU Present control",WS_OVERLAPPEDWINDOW|WS_VISIBLE,150,150,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
 if(!w)return 4;
 DXGI_SWAP_CHAIN_DESC sd={};sd.BufferDesc.Width=320;sd.BufferDesc.Height=240;sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=1;sd.OutputWindow=w;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
 ComPtr<ID3D11Device>dev;ComPtr<ID3D11DeviceContext>ctx;ComPtr<IDXGISwapChain>swap;D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0;
 HRESULT hr=D3D11CreateDeviceAndSwapChain(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&sd,&swap,&dev,nullptr,&ctx);
 printf("create=%08lx\n",hr);if(FAILED(hr))return 5;
 ComPtr<ID3D11Texture2D>frame;ComPtr<ID3D11RenderTargetView>view;
 hr=swap->GetBuffer(0,IID_PPV_ARGS(&frame));if(FAILED(hr))return 6;
 hr=dev->CreateRenderTargetView(frame.Get(),nullptr,&view);if(FAILED(hr))return 7;
 for(unsigned n=0;n<120;n++){
  MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
  float color[4]={0,0,0,1};color[n<40?0:n<80?2:1]=1;
  ctx->ClearRenderTargetView(view.Get(),color);
  hr=swap->Present(0,0);
  if(n%40==0||FAILED(hr))printf("present frame=%u hr=%08lx\n",n,hr);
  if(FAILED(hr)){
   HRESULT reason=dev->GetDeviceRemovedReason();
   printf("observed loss reason=%08lx expected=%u\n",reason,expectLoss);
   bool valid=expectLoss && (reason==DXGI_ERROR_DEVICE_REMOVED || reason==DXGI_ERROR_DEVICE_RESET || reason==DXGI_ERROR_DEVICE_HUNG || reason==DXGI_ERROR_DRIVER_INTERNAL_ERROR);
   view.Reset();frame.Reset();swap.Reset();ctx.Reset();dev.Reset();DestroyWindow(w);
   if(valid){puts("PASS injected device loss propagated to D3D application");return 0;}return 8;
  }
  Sleep(100);
 }
 if(expectLoss){puts("FAIL injected loss was not observed");return 12;}
 const float green[4]={0,1,0,1};ctx->ClearRenderTargetView(view.Get(),green);
 D3D11_TEXTURE2D_DESC td={};frame->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D>stage;hr=dev->CreateTexture2D(&td,nullptr,&stage);if(FAILED(hr))return 9;
 ctx->CopyResource(stage.Get(),frame.Get());D3D11_MAPPED_SUBRESOURCE m={};hr=ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m);if(FAILED(hr))return 10;
 unsigned bad=0;for(unsigned y=0;y<240;y++)for(unsigned x=0;x<320;x++){auto p=(const unsigned char*)m.pData+y*m.RowPitch+4*x;if(p[0]!=0||p[1]!=255||p[2]!=0||p[3]!=255)++bad;}
 ctx->Unmap(stage.Get(),0);printf("final green mismatches=%u/76800 removed=%08lx\n",bad,dev->GetDeviceRemovedReason());
 ctx->ClearState();ctx->Flush();stage.Reset();view.Reset();frame.Reset();swap.Reset();ctx.Reset();dev.Reset();DestroyWindow(w);
 if(bad)return 11;puts("PASS 120 native hosted Presents and final pixels");return 0;
}
