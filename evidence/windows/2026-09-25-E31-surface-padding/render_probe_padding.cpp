#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <share.h>
#include <vector>
#include <utility>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")
static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcW(w,m,a,b); }
static void displayConfig(FILE* log) {
 for (UINT flags : {UINT(QDC_ONLY_ACTIVE_PATHS),UINT(QDC_ALL_PATHS)}) {
  UINT paths=0,modes=0;LONG err=GetDisplayConfigBufferSizes(flags,&paths,&modes);
  fprintf(log,"displayconfig sizes flags %x err %ld paths %u modes %u\n",flags,err,paths,modes);
  if(err)continue;
  std::vector<DISPLAYCONFIG_PATH_INFO> p(paths?paths:1);
  std::vector<DISPLAYCONFIG_MODE_INFO> m(modes?modes:1);
  err=QueryDisplayConfig(flags,&paths,p.data(),&modes,m.data(),nullptr);
  fprintf(log,"displayconfig query flags %x err %ld paths %u modes %u\n",flags,err,paths,modes);
  if(!err)for(UINT i=0;i<paths;i++)fprintf(log,"path src %u dst %u mode %u/%u flags %x refresh %u/%u\n",p[i].sourceInfo.id,p[i].targetInfo.id,p[i].sourceInfo.modeInfoIdx,p[i].targetInfo.modeInfoIdx,p[i].flags,p[i].targetInfo.refreshRate.Numerator,p[i].targetInfo.refreshRate.Denominator);
 }
 fflush(log);
}

static bool complete(FILE* log, ID3D11Device* device, ID3D11DeviceContext* context, const char* tag) {
 D3D11_QUERY_DESC desc={D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;
 HRESULT hr=device->CreateQuery(&desc,&query);if(FAILED(hr)){fprintf(log,"completion %s create %08lx\n",tag,hr);return false;}
 context->End(query.Get());context->Flush();ULONGLONG start=GetTickCount64();BOOL done=FALSE;
 do {
  hr=context->GetData(query.Get(),&done,sizeof(done),0);
  if(FAILED(hr)){fprintf(log,"completion %s GetData %08lx\n",tag,hr);return false;}
  if(hr==S_OK&&done){fprintf(log,"completion %s PASS ms=%llu\n",tag,GetTickCount64()-start);fflush(log);return true;}
  Sleep(1);
 }while(GetTickCount64()-start<5000);
 fprintf(log,"completion %s TIMEOUT hr=%08lx done=%u\n",tag,hr,done);fflush(log);return false;
}
static bool moduleWitness(FILE* log) {
 HMODULE module=GetModuleHandleW(L"bc250d3d.dll");wchar_t path[32768];
 DWORD length=module?GetModuleFileNameW(module,path,32768):0;
 if(!length||length>=32768){fprintf(log,"UMD_MODULE_FAIL error=%lu\n",GetLastError());fflush(log);return false;}
 fprintf(log,"UMD_MODULE=%ls\n",path);fflush(log);return true;
}

static bool sharedControl(FILE* log, ID3D11Device* a, ID3D11DeviceContext* ca, UINT width, UINT height) {
 ComPtr<ID3D11Device> b; ComPtr<ID3D11DeviceContext> cb;
 D3D_FEATURE_LEVEL level;
 HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&b,&level,&cb);
 fprintf(log,"shared second device %08lx\n",hr); if(FAILED(hr))return false;
 D3D11_TEXTURE2D_DESC d={};d.Width=width;d.Height=height;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
 ComPtr<ID3D11Texture2D> ta,tb;
 hr=a->CreateTexture2D(&d,nullptr,&ta);fprintf(log,"shared create %08lx\n",hr);if(FAILED(hr))return false;
 ComPtr<IDXGIResource> dx;hr=ta.As(&dx);if(FAILED(hr))return false;
 HANDLE handle=nullptr;hr=dx->GetSharedHandle(&handle);fprintf(log,"shared handle %08lx\n",hr);if(FAILED(hr))return false;
 hr=b->OpenSharedResource(handle,IID_PPV_ARGS(&tb));fprintf(log,"shared open %08lx\n",hr);fflush(log);if(FAILED(hr))return false;
 auto read=[&](ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* texture,unsigned char r,unsigned char blue,const char* tag) {
  D3D11_TEXTURE2D_DESC st=d;st.Usage=D3D11_USAGE_STAGING;st.BindFlags=0;st.CPUAccessFlags=D3D11_CPU_ACCESS_READ;st.MiscFlags=0;
  ComPtr<ID3D11Texture2D> copy;HRESULT result=dev->CreateTexture2D(&st,nullptr,&copy);if(FAILED(result))return false;
  ctx->CopyResource(copy.Get(),texture);D3D11_MAPPED_SUBRESOURCE m={};result=ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&m);if(FAILED(result)){fprintf(log,"shared map %08lx\n",result);return false;}
  unsigned bad=0;for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){const unsigned char* p=(const unsigned char*)m.pData+y*m.RowPitch+x*4;if(p[0]!=r||p[1]!=0||p[2]!=blue||p[3]!=255)bad++;}
  ctx->Unmap(copy.Get(),0);fprintf(log,"shared %s mismatches %u of %u extent %ux%u\n",tag,bad,width*height,width,height);fflush(log);return bad==0;
 };
 ComPtr<ID3D11RenderTargetView> va,vb;hr=a->CreateRenderTargetView(ta.Get(),nullptr,&va);if(FAILED(hr))return false;
 hr=b->CreateRenderTargetView(tb.Get(),nullptr,&vb);if(FAILED(hr))return false;
 const float red[]={1,0,0,1},blue[]={0,0,1,1};
 ca->ClearRenderTargetView(va.Get(),red);if(!complete(log,a,ca,"A-clear"))return false;bool first=read(b.Get(),cb.Get(),tb.Get(),255,0,"A-to-B red");
 cb->ClearRenderTargetView(vb.Get(),blue);if(!complete(log,b.Get(),cb.Get(),"B-clear"))return false;bool second=read(a,ca,ta.Get(),0,255,"B-to-A blue");
 return first&&second;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR commandLine,int) {
 FILE* log=nullptr; log=_fsopen("C:\\BC250\\m10\\surface-padding\\render-probe.txt","w",_SH_DENYNO); if(!log)return 1;
 displayConfig(log);
 WNDCLASSW wc={}; wc.lpfnWndProc=proc; wc.hInstance=instance; wc.lpszClassName=L"BC250D3DProbe"; RegisterClassW(&wc);
 HWND window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"E26 Direct3D positive control",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,640,480,nullptr,nullptr,instance,nullptr);
 DXGI_SWAP_CHAIN_DESC desc={}; desc.BufferDesc.Width=640; desc.BufferDesc.Height=480; desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=1; desc.OutputWindow=window; desc.Windowed=TRUE; desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
 ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr; IDXGISwapChain* swap=nullptr; D3D_FEATURE_LEVEL level;
 HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,&level,&context);
 fprintf(log,"create %08lx level %x\n",hr,SUCCEEDED(hr)?level:0); fflush(log); if(FAILED(hr)){fclose(log);return 2;}
 if(wcsstr(commandLine,L"fullscreen")) {
  hr=swap->SetFullscreenState(TRUE,nullptr);fprintf(log,"fullscreen enter %08lx\n",hr);fflush(log);
  if(SUCCEEDED(hr)) {
   hr=swap->ResizeBuffers(1,1920,1200,DXGI_FORMAT_B8G8R8A8_UNORM,0);fprintf(log,"fullscreen resize %08lx\n",hr);fflush(log);
   if(SUCCEEDED(hr)) {
    ComPtr<ID3D11Texture2D> frame;ComPtr<ID3D11RenderTargetView> view;
    hr=swap->GetBuffer(0,IID_PPV_ARGS(&frame));fprintf(log,"fullscreen buffer %08lx\n",hr);
    if(SUCCEEDED(hr))hr=device->CreateRenderTargetView(frame.Get(),nullptr,&view);
    fprintf(log,"fullscreen rtv %08lx\n",hr);fflush(log);
    if(SUCCEEDED(hr)){const float color[]={0,1,0,1};context->ClearRenderTargetView(view.Get(),color);hr=swap->Present(0,0);fprintf(log,"fullscreen present %08lx\n",hr);fflush(log);Sleep(3000);}
   }
  }
  HRESULT result=hr;hr=swap->SetFullscreenState(FALSE,nullptr);fprintf(log,"fullscreen leave %08lx\n",hr);fflush(log);
  swap->Release();context->Release();device->Release();DestroyWindow(window);fclose(log);return SUCCEEDED(result)?0:6;
 }
 bool moduleOk=moduleWitness(log);
 bool sharedOk=true; for (auto extent : {std::pair<UINT,UINT>{64,32},{1428,33},{1366,35},{1,1},{67,65}}) { if(!sharedControl(log,device,context,extent.first,extent.second)){sharedOk=false;break;} } fprintf(log,"shared control %s\n",sharedOk?"PASS":"FAIL");fflush(log);
 ID3D11Texture2D* texture=nullptr; hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&texture); fprintf(log,"buffer %08lx\n",hr); if(FAILED(hr)){fclose(log);return 3;}
 ID3D11RenderTargetView* target=nullptr; hr=device->CreateRenderTargetView(texture,nullptr,&target); fprintf(log,"rtv %08lx\n",hr); if(FAILED(hr)){fclose(log);return 4;}
 const float green[]={0,1,0,1}; context->ClearRenderTargetView(target,green);
 D3D11_TEXTURE2D_DESC td={}; texture->GetDesc(&td); td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ; td.MiscFlags=0;
 ID3D11Texture2D* staging=nullptr; hr=device->CreateTexture2D(&td,nullptr,&staging); fprintf(log,"staging %08lx\n",hr);
 bool ok=false;
 if(SUCCEEDED(hr)){context->CopyResource(staging,texture); if(!complete(log,device,context,"green-copy")){fclose(log);return 7;} D3D11_MAPPED_SUBRESOURCE map={}; hr=context->Map(staging,0,D3D11_MAP_READ,0,&map); fprintf(log,"map %08lx\n",hr); if(SUCCEEDED(hr)){unsigned mismatches=0; for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++){const unsigned char* p=(const unsigned char*)map.pData+y*map.RowPitch+x*4; if(p[0]!=0||p[1]!=255||p[2]!=0||p[3]!=255)mismatches++;} fprintf(log,"green mismatches %u of 307200\n",mismatches); ok=mismatches==0;context->Unmap(staging,0);} staging->Release();}
 for(unsigned n=0;n<60;n++){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);} context->ClearRenderTargetView(target,green);hr=swap->Present(0,0);if(n==0||FAILED(hr)){fprintf(log,"present %u %08lx\n",n,hr);fflush(log);}if(FAILED(hr))break;Sleep(33);}
 fprintf(log,"device removed %08lx\n",device->GetDeviceRemovedReason()); target->Release();texture->Release();swap->Release();context->Release();device->Release();DestroyWindow(window);fclose(log);return ok&&sharedOk&&moduleOk&&SUCCEEDED(hr)?0:5;
}

