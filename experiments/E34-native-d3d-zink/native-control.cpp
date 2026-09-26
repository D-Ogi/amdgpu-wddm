#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include "tri_vs_4_0.h"
#include "tri_ps_4_0.h"
using Microsoft::WRL::ComPtr;
#define CHECK(expr) do { HRESULT h=(expr); printf("%s: %08lx\n",#expr,h); fflush(stdout); if(FAILED(h)) return 2; } while(0)
int main(int argc,char** argv) {
 setvbuf(stdout,nullptr,_IONBF,0);
 ComPtr<IDXGIFactory1> factory; CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 uint64_t luid=0; unsigned matches=0;
 for(UINT n=0;;n++) { ComPtr<IDXGIAdapter1> a; if(factory->EnumAdapters1(n,&a)==DXGI_ERROR_NOT_FOUND)break;
  DXGI_ADAPTER_DESC1 d={}; CHECK(a->GetDesc1(&d));
  if(d.VendorId==0x1002 && d.DeviceId==0x13fe) { memcpy(&luid,&d.AdapterLuid,8); matches++; }
  printf("adapter vendor=%04x device=%04x software=%u\n",d.VendorId,d.DeviceId,!!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE));
 }
 if(matches!=1||!luid){printf("expected one BC250 adapter; matches=%u\n",matches);return 3;}
 char text[32];sprintf_s(text,"%016llx",(unsigned long long)luid);SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",text);
 HMODULE umd=LoadLibraryA(argc>1?argv[1]:"bc250d3d_zink.dll");if(!umd){printf("LoadLibrary error=%lu\n",GetLastError());return 4;}
 ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_10_0;
 CHECK(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_SOFTWARE,umd,0,&level,1,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
 const char* modules[]={"bc250d3d_zink.dll","vulkan-1.dll","vulkan_radeon.dll"};
 for(auto name:modules){HMODULE m=GetModuleHandleA(name);char path[MAX_PATH]={};if(m)GetModuleFileNameA(m,path,MAX_PATH);printf("module %s: %s\n",name,path);}
 D3D11_TEXTURE2D_DESC d={};d.Width=d.Height=64;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET;
 ComPtr<ID3D11Texture2D> target,copy;CHECK(dev->CreateTexture2D(&d,nullptr,&target));
 d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(dev->CreateTexture2D(&d,nullptr,&copy));
 ComPtr<ID3D11RenderTargetView> view;CHECK(dev->CreateRenderTargetView(target.Get(),nullptr,&view));
 auto read=[&](unsigned red,unsigned blue){ctx->CopyResource(copy.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m={};HRESULT hr=ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&m);printf("map=%08lx\n",hr);if(FAILED(hr))return false;
 unsigned bad=0;for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){auto q=(const unsigned char*)m.pData+y*m.RowPitch+x*4;if(q[0]!=red||q[1]!=0||q[2]!=blue||q[3]!=255)bad++;}
 printf("expected RGBA=%u,0,%u,255 mismatches=%u/4096\n",red,blue,bad);ctx->Unmap(copy.Get(),0);return bad==0;};
 const float blue[]={0,0,1,1};ctx->ClearRenderTargetView(view.Get(),blue);if(!read(0,255))return 5;
 ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;CHECK(dev->CreateVertexShader(g_VS,sizeof(g_VS),nullptr,&vs));CHECK(dev->CreatePixelShader(g_PS,sizeof(g_PS),nullptr,&ps));
 D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
 ComPtr<ID3D11InputLayout> il;CHECK(dev->CreateInputLayout(layout,2,g_VS,sizeof(g_VS),&il));
 struct Vertex {float pos[4],color[4];};Vertex vertices[]={{{-1,-1,0.5f,1},{1,0,0,1}},{{3,-1,0.5f,1},{1,0,0,1}},{{-1,3,0.5f,1},{1,0,0,1}}};
 D3D11_BUFFER_DESC bd={};bd.ByteWidth=sizeof(vertices);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init={};init.pSysMem=vertices;
 ComPtr<ID3D11Buffer> vb;CHECK(dev->CreateBuffer(&bd,&init,&vb));UINT stride=sizeof(Vertex),offset=0;ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset);ctx->IASetInputLayout(il.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_RASTERIZER_DESC rs={};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;CHECK(dev->CreateRasterizerState(&rs,&raster));puts("before raster");ctx->RSSetState(raster.Get());puts("after raster");
 D3D11_VIEWPORT vp={0,0,64,64,0,1};puts("before viewport");ctx->RSSetViewports(1,&vp);puts("after viewport");puts("before vs");ctx->VSSetShader(vs.Get(),nullptr,0);puts("after vs");puts("before ps");ctx->PSSetShader(ps.Get(),nullptr,0);puts("after ps");puts("before rt");ctx->OMSetRenderTargets(1,view.GetAddressOf(),nullptr);puts("after rt");puts("before draw");ctx->Draw(3,0);puts("after draw");
 if(!read(255,0))return 6;CHECK(dev->GetDeviceRemovedReason());ctx->ClearState();ctx->Flush();printf("PASS native D3D clear and shader draw readback\n");return 0;
}
