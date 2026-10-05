#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
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
struct AuditReader {
 HANDLE file=INVALID_HANDLE_VALUE;
 std::string pending;
 ~AuditReader(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
 bool Open(const wchar_t *path){file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);return file!=INVALID_HANDLE_VALUE;}
 bool SeekEnd(){LARGE_INTEGER zero={};pending.clear();return SetFilePointerEx(file,zero,nullptr,FILE_END)!=FALSE;}
 bool Ack(unsigned marker){
  const std::string needle=" marker="+std::to_string(marker)+" ";
  char buffer[4096];DWORD count=0;
  while(ReadFile(file,buffer,sizeof(buffer),&count,nullptr)&&count){
   pending.append(buffer,count);
   size_t end;
   while((end=pending.find('\n'))!=std::string::npos){
    std::string line=pending.substr(0,end);pending.erase(0,end+1);
    if(line.find("BC250 audit lifetime event=checkpoint ")!=std::string::npos&&line.find(needle)!=std::string::npos)return true;
   }
   if(pending.size()>65536){puts("FAIL audit line exceeds bound");return false;}
  }
  return false;
 }
};
static bool Checkpoint(ID3D11DeviceContext *ctx,AuditReader &reader,const wchar_t *path,unsigned marker){
 if(!reader.SeekEnd())return false;
 std::wstring temp=std::wstring(path)+L".tmp";
 HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;
 std::string text=std::to_string(marker)+"\n";DWORD written=0;
 bool ok=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();
 if(ok)ok=FlushFileBuffers(file)!=FALSE;
 CloseHandle(file);
 if(!ok||!MoveFileExW(temp.c_str(),path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
 const ULONGLONG deadline=GetTickCount64()+5000;
 do{
  ctx->Flush();Pump();
  if(reader.Ack(marker)){printf("checkpoint_ack marker=%u tick=%llu\n",marker,GetTickCount64());return true;}
  Sleep(10);
 }while(GetTickCount64()<deadline);
 printf("FAIL checkpoint timeout marker=%u\n",marker);return false;
}
static bool Pixels(ID3D11DeviceContext *ctx,ID3D11Texture2D *frame,ID3D11Texture2D *readback,UINT expected){
 ctx->CopyResource(readback,frame);D3D11_MAPPED_SUBRESOURCE mapped={};
 if(!Check(ctx->Map(readback,0,D3D11_MAP_READ,0,&mapped),"readback Map"))return false;
 UINT bad=0;
 if(mapped.RowPitch<RowBytes)bad=Width*Height;
 else for(UINT y=0;y<Height;y++)for(UINT x=0;x<Width;x++){
  UINT value;memcpy(&value,static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4,4);bad+=value!=expected;
 }
 ctx->Unmap(readback,0);printf("pixels expected=%08x bad=%u total=%u\n",expected,bad,Width*Height);return !bad;
}
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
int wmain(int argc,wchar_t **argv){
 setvbuf(stdout,nullptr,_IONBF,0);
 if(argc==2&&!wcscmp(argv[1],L"--help")){puts("runtime-audit-control.exe MARKER_PATH STDERR_LOG_PATH [--update-subresource|--texture-draw] (interactive lab only; requires audit UMD and external watchdog)");return 0;}
 const bool textured=argc==4&&!wcscmp(argv[3],L"--texture-draw");
 const bool update=textured||(argc==4&&!wcscmp(argv[3],L"--update-subresource"));
 if(argc!=3&&!update){puts("FAIL arguments; use --help");return 1;}
 if(GetFileAttributesW(argv[1])!=INVALID_FILE_ATTRIBUTES){puts("FAIL marker path already exists; require fresh run directory");return 2;}
 AuditReader reader;if(!reader.Open(argv[2])){puts("FAIL audit log open");return 3;}
 if(!SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1")||!SetEnvironmentVariableA("BC250_HOST_AUDIT","1")||
    !SetEnvironmentVariableA("BC250_UPLOAD_AUDIT","1")||!SetEnvironmentVariableW(L"BC250_AUDIT_MARKER",argv[1]))return 4;
 ComPtr<IDXGIFactory1> factory;if(!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory"))return 5;
 ComPtr<IDXGIAdapter1> adapter;UINT matches=0;
 for(UINT n=0;;n++){
  ComPtr<IDXGIAdapter1> candidate;HRESULT hr=factory->EnumAdapters1(n,&candidate);
  if(hr==DXGI_ERROR_NOT_FOUND)break;if(!Check(hr,"EnumAdapters1"))return 6;
  DXGI_ADAPTER_DESC1 desc={};if(!Check(candidate->GetDesc1(&desc),"GetDesc1"))return 7;
  if(desc.VendorId==0x1002&&desc.DeviceId==0x13fe){
   adapter=candidate;matches++;unsigned long long luid=0;memcpy(&luid,&desc.AdapterLuid,8);
   char text[32];sprintf_s(text,"%016llx",luid);if(!SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",text))return 8;
  }
 }
 if(matches!=1){printf("FAIL target adapter count=%u\n",matches);return 9;}
 WNDCLASSW wc={};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"BC250AuditControl";
 if(!RegisterClassW(&wc))return 10;
 RECT rect={0,0,Width,Height};if(!AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE))return 11;
 HWND window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"G0 GPU and deliberate CPU-copy control",WS_OVERLAPPEDWINDOW|WS_VISIBLE,150,150,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
 if(!window)return 12;
 DXGI_SWAP_CHAIN_DESC sd={};sd.BufferDesc.Width=Width;sd.BufferDesc.Height=Height;sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
 sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.OutputWindow=window;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
 ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ComPtr<IDXGISwapChain> swap;D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0;
 if(!Check(D3D11CreateDeviceAndSwapChain(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&fl,1,D3D11_SDK_VERSION,&sd,&swap,&dev,nullptr,&ctx),"create flip"))return 13;
 DXGI_SWAP_CHAIN_DESC actual={};if(!Check(swap->GetDesc(&actual),"swap desc"))return 14;
 if(actual.SwapEffect!=sd.SwapEffect||actual.BufferCount!=2||actual.BufferDesc.Width!=Width||actual.BufferDesc.Height!=Height||actual.BufferDesc.Format!=sd.BufferDesc.Format)return 15;
 ComPtr<ID3D11Texture2D> frame,upload,readback;ComPtr<ID3D11RenderTargetView> view;
 if(!Check(swap->GetBuffer(0,IID_PPV_ARGS(&frame)),"GetBuffer")||!Check(dev->CreateRenderTargetView(frame.Get(),nullptr,&view),"RTV"))return 16;
 D3D11_TEXTURE2D_DESC td={};frame->GetDesc(&td);td.Usage=update?D3D11_USAGE_DEFAULT:D3D11_USAGE_STAGING;td.BindFlags=update?D3D11_BIND_SHADER_RESOURCE:0;td.MiscFlags=0;td.CPUAccessFlags=update?0:D3D11_CPU_ACCESS_WRITE;
 if(!Check(dev->CreateTexture2D(&td,nullptr,&upload),"upload texture"))return 17;
 td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;if(!Check(dev->CreateTexture2D(&td,nullptr,&readback),"readback texture"))return 18;
 TextureDraw draw;if(textured&&!draw.Init(dev.Get()))return 30;
 const float black[4]={0,0,0,1};ctx->ClearRenderTargetView(view.Get(),black);
 if(!Check(swap->Present(0,0),"warmup Present")||!Checkpoint(ctx.Get(),reader,argv[1],1))return 19;
 for(UINT n=0;n<16;n++){
  Pump();float color[4]={0,0,0,1};color[n%3]=1;
  if(textured)draw.Draw(ctx.Get(),view.Get(),n%2);else ctx->ClearRenderTargetView(view.Get(),color);
  if(!Check(swap->Present(0,0),"GPU Present"))return 20;Sleep(30);
 }
 if(!Checkpoint(ctx.Get(),reader,argv[1],2)||!Checkpoint(ctx.Get(),reader,argv[1],3))return 21;
 if(textured){
  draw.Draw(ctx.Get(),view.Get(),1);
  if(!Pixels(ctx.Get(),frame.Get(),readback.Get(),0xffff00ff))return 31;
  draw.Draw(ctx.Get(),view.Get(),0);
 }else{const float green[4]={0,1,0,1};ctx->ClearRenderTargetView(view.Get(),green);}
 if(!Pixels(ctx.Get(),frame.Get(),readback.Get(),0xff00ff00)||!Checkpoint(ctx.Get(),reader,argv[1],4))return 22;
 if(textured){ctx->ClearState();printf("gpu_work=texture_draw draws=18\n");}
 std::vector<UINT> pixels(Width*Height);unsigned long long copied=0;
 if(!Checkpoint(ctx.Get(),reader,argv[1],5))return 23;
 for(UINT n=0;n<12;n++){
  Pump();std::fill(pixels.begin(),pixels.end(),(n&1)?0xff00ffffu:0xffff00ffu);
  if(update){
   ctx->UpdateSubresource(upload.Get(),0,nullptr,pixels.data(),RowBytes,RowBytes*Height);
   copied+=static_cast<unsigned long long>(RowBytes)*Height;
  }else{
   D3D11_MAPPED_SUBRESOURCE mapped={};if(!Check(ctx->Map(upload.Get(),0,D3D11_MAP_WRITE,0,&mapped),"upload Map"))return 24;
   if(mapped.RowPitch<RowBytes){ctx->Unmap(upload.Get(),0);return 25;}
   for(UINT y=0;y<Height;y++){memcpy(static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,pixels.data()+y*Width,RowBytes);copied+=RowBytes;}
   ctx->Unmap(upload.Get(),0);
  }
  ctx->CopyResource(frame.Get(),upload.Get());
  if(!Check(swap->Present(0,0),"CPU-copy Present"))return 26;Sleep(30);
 }
 printf("copy_api=%s\n",update?"UpdateSubresource":"Map");
 printf("deliberate_cpu_copy frames=12 bytes=%llu\n",copied);
 if(copied!=12ULL*RowBytes*Height||!Checkpoint(ctx.Get(),reader,argv[1],6)||!Checkpoint(ctx.Get(),reader,argv[1],7))return 27;
 ctx->CopyResource(frame.Get(),upload.Get());
 if(!Pixels(ctx.Get(),frame.Get(),readback.Get(),0xff00ffff)||!Checkpoint(ctx.Get(),reader,argv[1],8))return 28;
 if(!Check(dev->GetDeviceRemovedReason(),"device status"))return 29;
 ctx->ClearState();ctx->Flush();view.Reset();frame.Reset();upload.Reset();readback.Reset();swap.Reset();ctx.Reset();dev.Reset();DestroyWindow(window);
 puts(textured?"PASS GPU texture draws, deliberate CPU-copy frames, isolated readbacks and8 acknowledged checkpoints":"PASS GPU clears, deliberate CPU-copy frames, isolated readbacks and8 acknowledged checkpoints");return 0;
}
