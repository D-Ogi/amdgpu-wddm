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
 HRESULT hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&dev,nullptr,&ctx);printf("hardware D3D11CreateDevice=%08lx\n",hr);if(FAILED(hr))return 3;
 for(auto name:{"bc250d3d_zink.dll","vulkan_radeon.dll"}){auto m=GetModuleHandleA(name);char p[MAX_PATH]={};if(m)GetModuleFileNameA(m,p,MAX_PATH);printf("module %s: %s\n",name,p);if(!m)return 4;}
 UINT flags[]={0,D3D11_RESOURCE_MISC_SHARED,D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX};
 unsigned created=0;
 for(UINT misc:flags){D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=64;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=misc;ComPtr<ID3D11Texture2D>tex;hr=dev->CreateTexture2D(&td,nullptr,&tex);printf("CreateTexture2D misc=%x hr=%08lx\n",misc,hr);if(SUCCEEDED(hr))created++;}
 ctx.Reset();dev.Reset();printf("runtime inventory complete created=%u/3\n",created);return created==3?0:5;
}
