#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dbghelp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>
using Microsoft::WRL::ComPtr;
static LONG WINAPI dumpFailure(EXCEPTION_POINTERS *info){
 char path[MAX_PATH]={};GetModuleFileNameA(nullptr,path,MAX_PATH);char *slash=strrchr(path,'\\');if(!slash)return EXCEPTION_EXECUTE_HANDLER;strcpy_s(slash+1,MAX_PATH-(slash+1-path),"texture-crash.dmp");
 HANDLE f=CreateFileA(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(f!=INVALID_HANDLE_VALUE){MINIDUMP_EXCEPTION_INFORMATION e={GetCurrentThreadId(),info,FALSE};MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),f,MiniDumpNormal,&e,nullptr,nullptr);CloseHandle(f);}return EXCEPTION_EXECUTE_HANDLER;
}
int main(){
 SetUnhandledExceptionFilter(dumpFailure);
 setvbuf(stdout,nullptr,_IONBF,0);
 SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1");
 ComPtr<IDXGIFactory1> f;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))return 1;
 ComPtr<IDXGIAdapter1> adapter;
 for(UINT n=0;;n++){ComPtr<IDXGIAdapter1>a;if(f->EnumAdapters1(n,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d={};a->GetDesc1(&d);if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=a;unsigned long long luid=0;memcpy(&luid,&d.AdapterLuid,8);char t[32];sprintf_s(t,"%016llx",luid);SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",t);}}
 if(!adapter)return 2;
 D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext>ctx;
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
  const char *vs="float4 main(uint id:SV_VertexID):SV_Position { float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }";
  const char *ps="Texture2D<float4> tex:register(t0); SamplerState samp:register(s0); float4 main():SV_Target { return tex.Sample(samp,float2(2.25,2.25)); }";
  ComPtr<ID3DBlob> vb,pb,error;
  h=D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_4_0",0,0,&vb,&error);if(FAILED(h))return false;
  h=D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_4_0",0,0,&pb,&error);if(FAILED(h))return false;
  ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
  if(FAILED(device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vertex)))return false;
  if(FAILED(device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&pixel)))return false;
  D3D11_TEXTURE2D_DESC source={};source.Width=source.Height=1;source.ArraySize=source.MipLevels=1;source.Format=DXGI_FORMAT_R8G8B8A8_UNORM;source.SampleDesc.Count=1;source.Usage=D3D11_USAGE_IMMUTABLE;source.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  unsigned char colors[2][4]={{255,255,255,255},{0,0,0,255}};colors[1][channel]=255;
  ComPtr<ID3D11Texture2D> textures[2];ComPtr<ID3D11ShaderResourceView> views[2];
  for(unsigned i=0;i<2;i++){D3D11_SUBRESOURCE_DATA initial={colors[i],4,4};if(FAILED(device->CreateTexture2D(&source,&initial,&textures[i])))return false;if(FAILED(device->CreateShaderResourceView(textures[i].Get(),nullptr,&views[i])))return false;}
  D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;sd.ComparisonFunc=D3D11_COMPARISON_NEVER;sd.MaxAnisotropy=1;
  ComPtr<ID3D11SamplerState> samplers[2];if(FAILED(device->CreateSamplerState(&sd,&samplers[0])))return false;
  sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;sd.BorderColor[0]=sd.BorderColor[1]=sd.BorderColor[2]=sd.BorderColor[3]=1;
  if(FAILED(device->CreateSamplerState(&sd,&samplers[1])))return false;
  ID3D11ShaderResourceView *srv[]={views[0].Get(),views[1].Get()};ID3D11SamplerState *ss[]={samplers[0].Get(),samplers[1].Get()};ID3D11RenderTargetView *rt=view.Get();
  D3D11_VIEWPORT viewport={0,0,64,64,0,1};context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&rt,nullptr);
  context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);context->PSSetShaderResources(0,2,srv);context->PSSetSamplers(0,2,ss);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->Draw(3,0);context->CopyResource(staging.Get(),target.Get());context->ClearState();
  D3D11_MAPPED_SUBRESOURCE m={};h=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m);if(FAILED(h))return false;
  unsigned bad=0;for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){auto p=(const unsigned char*)m.pData+y*m.RowPitch+x*4;for(unsigned c=0;c<4;c++)if(p[c]!=(c==channel||c==3?255:0)){bad++;break;}}
  context->Unmap(staging.Get(),0);printf("channel=%u mismatches=%u/4096\n",channel,bad);return bad==0;
 };
 if(!readColor(dev.Get(),ctx.Get(),0)||!readColor(second.Get(),secondCtx.Get(),2))return 6;
 ctx->ClearState();ctx->Flush();ctx.Reset();dev.Reset();puts("first device destroyed; checking second");
 if(!readColor(second.Get(),secondCtx.Get(),1))return 7;
 secondCtx->ClearState();secondCtx->Flush();secondCtx.Reset();second.Reset();puts("PASS independent texture1 sampler0 pixel oracle and two-device lifetime");return 0;
}
