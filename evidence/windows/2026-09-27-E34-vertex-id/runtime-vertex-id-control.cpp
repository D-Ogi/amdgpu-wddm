#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using Microsoft::WRL::ComPtr;
int main(int argc,char **argv) {
 setvbuf(stdout,nullptr,_IONBF,0);
 bool warp=argc>1 && !strcmp(argv[1],"--warp");
 SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1");
 ComPtr<IDXGIFactory1> factory;ComPtr<IDXGIAdapter1> adapter;
 if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 1;
 if(!warp)for(UINT n=0;;++n){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(n,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d={};a->GetDesc1(&d);if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=a;unsigned long long luid=0;memcpy(&luid,&d.AdapterLuid,8);char text[32];sprintf_s(text,"%016llx",luid);SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",text);break;}}
 if(!warp&&!adapter)return 2;
 D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext>ctx;
 HRESULT hr=D3D11CreateDevice(adapter.Get(),warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&dev,nullptr,&ctx);
 printf("mode=%s create=%08lx\n",warp?"WARP":"hardware",hr);if(FAILED(hr))return 3;
 for(auto name:{"bc250d3d_zink.dll","bc250d3d.dll","d3d10warp.dll"}){auto m=GetModuleHandleA(name);char p[MAX_PATH]={};if(m)GetModuleFileNameA(m,p,MAX_PATH);printf("module %s: %s\n",name,p);}
 const char *vs="struct O {float4 p:SV_Position; nointerpolation uint id:TEXCOORD0;}; O main(uint id:SV_VertexID){O o;o.p=float4(-1.0+(float(id%3)+0.5)*(2.0/3.0),0,0,1);o.id=id;return o;}";
 const char *ps="struct O {float4 p:SV_Position; nointerpolation uint id:TEXCOORD0;}; float4 main(O i):SV_Target{return float4(i.id,2,3,4);}";
 ComPtr<ID3DBlob>vb,pb,error;ComPtr<ID3D11VertexShader>vertex;ComPtr<ID3D11PixelShader>pixel;
 if(FAILED(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_4_0",0,0,&vb,&error)))return 4;
 if(FAILED(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_4_0",0,0,&pb,&error)))return 5;
 if(FAILED(dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vertex)))return 6;
 if(FAILED(dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&pixel)))return 7;
 D3D11_TEXTURE2D_DESC td={};td.Width=3;td.Height=1;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 ComPtr<ID3D11Texture2D>target,staging;ComPtr<ID3D11RenderTargetView>view;
 if(FAILED(dev->CreateTexture2D(&td,nullptr,&target))||FAILED(dev->CreateRenderTargetView(target.Get(),nullptr,&view)))return 8;
 td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 if(FAILED(dev->CreateTexture2D(&td,nullptr,&staging)))return 9;
 unsigned short indices[3]={2,0,1};D3D11_BUFFER_DESC bd={};bd.ByteWidth=sizeof(indices);bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_INDEX_BUFFER;D3D11_SUBRESOURCE_DATA init={indices,0,0};ComPtr<ID3D11Buffer>ib;if(FAILED(dev->CreateBuffer(&bd,&init,&ib)))return 10;
 D3D11_VIEWPORT vp={0,0,3,1,0,1};ctx->RSSetViewports(1,&vp);auto rt=view.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);ctx->VSSetShader(vertex.Get(),nullptr,0);ctx->PSSetShader(pixel.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);
 unsigned total=0;
 for(unsigned mode=0;mode<3;mode++){
  float clear[4]={-100,-100,-100,-100};ctx->ClearRenderTargetView(view.Get(),clear);
  if(mode==2)ctx->DrawIndexed(3,0,7);else ctx->Draw(3,mode==1?5:0);
  ctx->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m={};hr=ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m);if(FAILED(hr)){printf("map=%08lx\n",hr);return 11;}
  auto p=(const float*)m.pData;unsigned bad=0;printf("case=%u actual=",mode);
  for(unsigned x=0;x<3;x++){printf(" %.0f",p[x*4]);if(p[x*4]!=float(x)||p[x*4+1]!=2||p[x*4+2]!=3||p[x*4+3]!=4)bad++;}
  ctx->Unmap(staging.Get(),0);printf(" mismatches=%u/3\n",bad);total+=bad;
 }
 ctx->ClearState();ctx->Flush();printf("total_mismatches=%u\n",total);return total?12:0;
}

