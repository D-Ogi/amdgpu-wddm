#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>
using Microsoft::WRL::ComPtr;
int main(int argc,char**){
 const bool baseline=argc>1;
 setvbuf(stdout,nullptr,_IONBF,0);
 if(!baseline){SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1");SetEnvironmentVariableA("BC250_D3D_RUNTIME_INVENTORY","1");}
 ComPtr<IDXGIFactory1> f;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))return 1;
 ComPtr<IDXGIAdapter1> adapter;
 for(UINT n=0;;n++){ComPtr<IDXGIAdapter1>a;if(f->EnumAdapters1(n,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d={};a->GetDesc1(&d);if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=a;unsigned long long luid=0;memcpy(&luid,&d.AdapterLuid,8);char t[32];sprintf_s(t,"%016llx",luid);SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",t);}}
 if(!adapter)return 2;
 D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext>ctx;
 HRESULT hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&dev,nullptr,&ctx);printf("hardware D3D11CreateDevice=%08lx\n",hr);if(FAILED(hr))return 3;
 const char* names[]={baseline?"bc250d3d.dll":"bc250d3d_zink.dll"};
 for(auto name:names){auto m=GetModuleHandleA(name);char p[MAX_PATH]={};if(m)GetModuleFileNameA(m,p,MAX_PATH);printf("module %s: %s\n",name,p);if(!m)return 4;}

 D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=64;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
 ComPtr<ID3D11Texture2D>tex;hr=dev->CreateTexture2D(&td,nullptr,&tex);printf("CreateTexture2D shared=%08lx\n",hr);if(FAILED(hr))return 5;
 ComPtr<IDXGIResource>resource;hr=tex.As(&resource);printf("Query IDXGIResource=%08lx\n",hr);if(FAILED(hr))return 6;
 HANDLE handle=nullptr;hr=resource->GetSharedHandle(&handle);printf("GetSharedHandle=%08lx nonzero=%u\n",hr,handle!=nullptr);if(FAILED(hr)||!handle)return 7;
 ComPtr<ID3D11Device>other;ComPtr<ID3D11DeviceContext>otherCtx;
 hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&other,nullptr,&otherCtx);printf("second hardware device=%08lx\n",hr);if(FAILED(hr))return 8;
 ComPtr<ID3D11Texture2D>opened;hr=other->OpenSharedResource(handle,IID_PPV_ARGS(&opened));printf("OpenSharedResource=%08lx\n",hr);if(FAILED(hr))return 9;
 D3D11_TEXTURE2D_DESC od={};opened->GetDesc(&od);printf("opened extent=%ux%u format=%u\n",od.Width,od.Height,od.Format);
 if(od.Width!=64||od.Height!=64||od.Format!=td.Format)return 10;
 opened.Reset();otherCtx.Reset();other.Reset();resource.Reset();tex.Reset();ctx.Reset();dev.Reset();puts("PASS shared handle queried and opened on second runtime device");return 0;
}
