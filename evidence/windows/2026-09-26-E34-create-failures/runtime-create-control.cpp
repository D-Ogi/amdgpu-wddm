#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>
using Microsoft::WRL::ComPtr;
int main(){
 setvbuf(stdout,nullptr,_IONBF,0);
 SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1");
 ComPtr<IDXGIFactory1> f;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))return 1;
 ComPtr<IDXGIAdapter1> adapter;
 for(UINT n=0;;n++){ComPtr<IDXGIAdapter1>a;if(f->EnumAdapters1(n,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d={};a->GetDesc1(&d);if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=a;unsigned long long luid=0;memcpy(&luid,&d.AdapterLuid,8);char t[32];sprintf_s(t,"%016llx",luid);SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",t);}}
 if(!adapter)return 2;
 D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext>ctx;
 for(auto stage:{"context","screen","pipe","buffer","cso","empty_vs","empty_fs"}) {
  SetEnvironmentVariableA("BC250_HOST_TEST_CREATE",stage);
  HRESULT failed=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&dev,nullptr,&ctx);
  printf("injected stage=%s result=%08lx device=%d context=%d\n",stage,failed,!!dev,!!ctx);
  if(failed!=E_OUTOFMEMORY || dev || ctx)return 20;
 }
 SetEnvironmentVariableA("BC250_HOST_TEST_CREATE",nullptr);
 HRESULT hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&dev,nullptr,&ctx);printf("hardware D3D11CreateDevice=%08lx\n",hr);if(FAILED(hr))return 3;
 for(auto name:{"bc250d3d_zink.dll","vulkan_radeon.dll"}){auto m=GetModuleHandleA(name);char p[MAX_PATH]={};if(m)GetModuleFileNameA(m,p,MAX_PATH);printf("module %s: %s\n",name,p);if(!m)return 4;}

 ComPtr<ID3D11Device> second;ComPtr<ID3D11DeviceContext> secondCtx;
 hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&second,nullptr,&secondCtx);printf("second hardware device=%08lx\n",hr);if(FAILED(hr))return 5;
 auto readColor=[](ID3D11Device* device,ID3D11DeviceContext* context,unsigned channel){
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=64;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
  ComPtr<ID3D11Texture2D>target,staging;ComPtr<ID3D11RenderTargetView>view;
  HRESULT h=device->CreateTexture2D(&td,nullptr,&target);if(FAILED(h))return false;
  h=device->CreateRenderTargetView(target.Get(),nullptr,&view);if(FAILED(h))return false;
  td.MiscFlags=0;td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  h=device->CreateTexture2D(&td,nullptr,&staging);if(FAILED(h))return false;
  float color[4]={0,0,0,1};color[channel]=1;context->ClearRenderTargetView(view.Get(),color);context->CopyResource(staging.Get(),target.Get());
  D3D11_MAPPED_SUBRESOURCE m={};h=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m);if(FAILED(h))return false;
  unsigned bad=0;for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){auto p=(const unsigned char*)m.pData+y*m.RowPitch+x*4;for(unsigned c=0;c<4;c++)if(p[c]!=(c==channel||c==3?255:0)){bad++;break;}}
  context->Unmap(staging.Get(),0);printf("channel=%u mismatches=%u/4096\n",channel,bad);return bad==0;
 };
 if(!readColor(dev.Get(),ctx.Get(),0)||!readColor(second.Get(),secondCtx.Get(),2))return 6;
 ctx->ClearState();ctx->Flush();ctx.Reset();dev.Reset();puts("first device destroyed; checking second");
 if(!readColor(second.Get(),secondCtx.Get(),1))return 7;
 secondCtx->ClearState();secondCtx->Flush();secondCtx.Reset();second.Reset();puts("PASS two native D3D devices with independent screen lifetimes");return 0;
}
