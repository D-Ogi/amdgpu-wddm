#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
using Microsoft::WRL::ComPtr;
static constexpr UINT Width=320, Height=240, RowBytes=Width*4;
static bool Check(HRESULT hr,const char *what) {
 if(FAILED(hr)){printf("FAIL %s hr=%08lx\n",what,hr);return false;}return true;
}
static LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
static void Pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
struct TextureDraw {
 ComPtr<ID3D11VertexShader> vertex;
 ComPtr<ID3D11PixelShader> pixel;
 ComPtr<ID3D11ShaderResourceView> views[2];
 ComPtr<ID3D11SamplerState> sampler;
 ComPtr<ID3D11RasterizerState> raster;
 bool Init(ID3D11Device *dev){
  const char *vs="float4 main(uint id:SV_VertexID):SV_Position { float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }";
  const char *ps="Texture2D<float4> tex:register(t0); SamplerState samp:register(s0); float4 main():SV_Target { return tex.Sample(samp,float2(0.5,0.5)); }";
  ComPtr<ID3DBlob> vb,pb,error;
  if(!Check(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_4_0",0,0,&vb,&error),"compile VS")||
     !Check(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_4_0",0,0,&pb,&error),"compile PS"))return false;
  if(!Check(dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vertex),"VS")||
     !Check(dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&pixel),"PS"))return false;
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=td.MipLevels=td.ArraySize=1;
  td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;
  td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  const UINT colors[2]={0xff00ff00,0xffff00ff};
  for(UINT i=0;i<2;i++){
   D3D11_SUBRESOURCE_DATA data={&colors[i],4,4};ComPtr<ID3D11Texture2D> texture;
   if(!Check(dev->CreateTexture2D(&td,&data,&texture),"sampled texture")||
      !Check(dev->CreateShaderResourceView(texture.Get(),nullptr,&views[i]),"SRV"))return false;
  }
  D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD=D3D11_FLOAT32_MAX;sd.MaxAnisotropy=1;sd.ComparisonFunc=D3D11_COMPARISON_NEVER;
  D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
  return Check(dev->CreateSamplerState(&sd,&sampler),"sampler")&&Check(dev->CreateRasterizerState(&rd,&raster),"raster");
 }
 void Draw(ID3D11DeviceContext *ctx,ID3D11RenderTargetView *target,UINT index){
  const float black[4]={0,0,0,1};ctx->ClearRenderTargetView(target,black);
  D3D11_VIEWPORT vp={0,0,static_cast<float>(Width),static_cast<float>(Height),0,1};
  ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster.Get());ctx->OMSetRenderTargets(1,&target,nullptr);
  ctx->VSSetShader(vertex.Get(),nullptr,0);ctx->PSSetShader(pixel.Get(),nullptr,0);
  ID3D11ShaderResourceView *srv=views[index].Get();ID3D11SamplerState *ss=sampler.Get();
  ctx->PSSetShaderResources(0,1,&srv);ctx->PSSetSamplers(0,1,&ss);
  ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->Draw(3,0);
 }
};

// External runner owns module attestation, STOP/thermal checks, captures and rollback.
static bool Receipt(const std::wstring &dir,HWND window,UINT frames,bool frozen,ULONGLONG started){
 RECT r={};POINT origin={};LARGE_INTEGER qpc={},frequency={};
 if(!GetClientRect(window,&r)||!ClientToScreen(window,&origin))return false;
 QueryPerformanceCounter(&qpc);QueryPerformanceFrequency(&frequency);
 char data[512];int size=sprintf_s(data,"{\"pid\":%lu,\"frames\":%u,\"frozen\":%s,\"expected_bgra\":%u,\"client\":[%ld,%ld,%ld,%ld],\"qpc\":%lld,\"frequency\":%lld,\"elapsed_ms\":%llu}\n",GetCurrentProcessId(),frames,frozen?"true":"false",0xff00ff00u,origin.x,origin.y,origin.x+r.right,origin.y+r.bottom,qpc.QuadPart,frequency.QuadPart,GetTickCount64()-started);
 if(size<=0)return false;
 std::wstring temp=dir+L"\\heartbeat.tmp",target=dir+L"\\heartbeat.json";
 HANDLE f=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(f==INVALID_HANDLE_VALUE)return false;
 DWORD wrote=0;bool ok=WriteFile(f,data,static_cast<DWORD>(size),&wrote,nullptr)&&wrote==static_cast<DWORD>(size);
 if(ok)ok=FlushFileBuffers(f)!=FALSE;CloseHandle(f);
 return ok&&MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
int wmain(int argc,wchar_t **argv){
 setvbuf(stdout,nullptr,_IONBF,0);
 if(argc==2&&!wcscmp(argv[1],L"--help")){puts("gpu-window-control.exe EXISTING_FRESH_DIRECTORY [--lower] (interactive lab only; external router/watchdog required; freeze and stop files; 120s hard process deadline)");return 0;}
 const bool lower=argc==3&&!wcscmp(argv[2],L"--lower");
 if(argc!=2&&!lower)return 1;
 const int windowY=lower?600:300;
 const std::wstring dir=argv[1];
 DWORD attr=GetFileAttributesW(dir.c_str());
 if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY))return 2;
 for(const wchar_t *n:{L"heartbeat.json",L"heartbeat.tmp",L"freeze",L"stop"})if(GetFileAttributesW((dir+L"\\"+n).c_str())!=INVALID_FILE_ATTRIBUTES)return 3;
 if(!SetProcessDPIAware())return 4;
 ComPtr<IDXGIFactory1> factory;if(!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory"))return 5;
 ComPtr<IDXGIAdapter1> adapter;UINT matches=0;
 for(UINT n=0;;n++){
  ComPtr<IDXGIAdapter1> a;HRESULT hr=factory->EnumAdapters1(n,&a);
  if(hr==DXGI_ERROR_NOT_FOUND)break;if(!Check(hr,"adapter"))return 6;
  DXGI_ADAPTER_DESC1 desc={};if(!Check(a->GetDesc1(&desc),"description"))return 7;
  if(desc.VendorId==0x1002&&desc.DeviceId==0x13fe&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){
   adapter=a;matches++;unsigned long long luid=0;memcpy(&luid,&desc.AdapterLuid,8);
   char text[32];sprintf_s(text,"%016llx",luid);
   if(!SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",text))return 8;
   printf("adapter vendor=%04x device=%04x luid=%s\n",desc.VendorId,desc.DeviceId,text);
  }
 }
 if(matches!=1)return 9;
 WNDCLASSW wc={};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"BC250GpuWindowControl";
 if(!RegisterClassW(&wc))return 10;
 HWND window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"G0 native GPU composition",WS_POPUP|WS_VISIBLE,600,windowY,Width,Height,nullptr,nullptr,wc.hInstance,nullptr);
 if(!window)return 11;
 DXGI_SWAP_CHAIN_DESC sd={};sd.BufferDesc.Width=Width;sd.BufferDesc.Height=Height;sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
 sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.OutputWindow=window;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
 ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ComPtr<IDXGISwapChain> swap;D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0,actualLevel={};
 if(!Check(D3D11CreateDeviceAndSwapChain(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&sd,&swap,&dev,&actualLevel,&ctx),"create flip")||actualLevel!=fl)return 12;
 DXGI_SWAP_CHAIN_DESC actual={};if(!Check(swap->GetDesc(&actual),"swap description"))return 13;
 if(actual.SwapEffect!=sd.SwapEffect||actual.BufferCount!=sd.BufferCount||actual.BufferDesc.Width!=Width||actual.BufferDesc.Height!=Height||actual.BufferDesc.Format!=sd.BufferDesc.Format)return 14;
 ComPtr<ID3D11Texture2D> frame;ComPtr<ID3D11RenderTargetView> view;
 if(!Check(swap->GetBuffer(0,IID_PPV_ARGS(&frame)),"back buffer")||!Check(dev->CreateRenderTargetView(frame.Get(),nullptr,&view),"RTV"))return 15;
 TextureDraw draw;if(!draw.Init(dev.Get()))return 16;
 const ULONGLONG start=GetTickCount64();UINT frames=0;bool frozen=false,stopped=false;
 while(GetTickCount64()-start<120000){
  Pump();
  if(GetFileAttributesW((dir+L"\\stop").c_str())!=INVALID_FILE_ATTRIBUTES){stopped=true;break;}
  bool freeze=GetFileAttributesW((dir+L"\\freeze").c_str())!=INVALID_FILE_ATTRIBUTES;
  if(!frozen){
   if(!freeze&&!SetWindowPos(window,nullptr,600+static_cast<int>((frames/10)%8)*8,windowY,Width,Height,SWP_NOZORDER|SWP_NOACTIVATE))return 17;
   draw.Draw(ctx.Get(),view.Get(),freeze?0:(frames/10)%2);
   HRESULT hr=swap->Present(1,0);if(hr!=S_OK){printf("FAIL Present hr=%08lx\n",hr);return 18;}
   frames++;
   if(freeze){if(!Check(DwmFlush(),"freeze DwmFlush"))return 19;frozen=true;}
  }
  if(frames%10==0||frozen)if(!Receipt(dir,window,frames,frozen,start))return 20;
  Sleep(30);
 }
 if(!stopped){puts("FAIL external stop missing before120s deadline");return 21;}
 if(!frozen||frames<2){puts("FAIL freeze/animation missing");return 22;}
 if(!Check(dev->GetDeviceRemovedReason(),"device status"))return 23;
 printf("PASS gpu window frames=%u frozen=1 no_readback=1\n",frames);
 ctx->ClearState();ctx->Flush();view.Reset();frame.Reset();swap.Reset();ctx.Reset();dev.Reset();DestroyWindow(window);
 return 0;
}
