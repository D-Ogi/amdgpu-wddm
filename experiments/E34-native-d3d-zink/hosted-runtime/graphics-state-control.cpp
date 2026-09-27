#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
static void require(bool value, const char* message) {
   if (!value) throw std::runtime_error(message);
}
static void check(HRESULT value, const char* message) {
   if (FAILED(value)) { printf("%s=%08lx\n", message, value); throw std::runtime_error(message); }
}
static const char shaders[] = R"(
cbuffer Parameters : register(b0) { float4 color; float depth; float3 padding; };
float4 VS(uint id : SV_VertexID) : SV_Position {
   float2 p = id == 0 ? float2(-1,-1) : (id == 1 ? float2(3,-1) : float2(-1,3));
   return float4(p, depth, 1);
}
float4 Solid(float4 p : SV_Position) : SV_Target { return color; }
float4 Branch(float4 p : SV_Position) : SV_Target {
   [branch] if (p.x < 32) return float4(1,0,0,1);
   else return float4(0,1,0,1);
}
Texture2D<float4> image : register(t0);
float4 Fetch(float4 p : SV_Position) : SV_Target { return image.Load(int3(int2(p.xy),0)); }
)";
static ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
   ComPtr<ID3DBlob> blob, error;
   HRESULT hr = D3DCompile(shaders, sizeof(shaders)-1, "graphics-state-control", nullptr,
      nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_SKIP_OPTIMIZATION,
      0, &blob, &error);
   if (error) printf("compiler %s: %s\n", entry, static_cast<const char*>(error->GetBufferPointer()));
   check(hr, "D3DCompile"); return blob;
}
int main(int argc, char** argv) {
   setvbuf(stdout, nullptr, _IONBF, 0);
   try {
      require(argc == 2 || (argc == 3 && !strcmp(argv[2], "batch")), "usage: graphics-state-control baseline|warp|hosted [batch]");
      const bool hosted = !strcmp(argv[1], "hosted"), warp = !strcmp(argv[1], "warp");
      require(hosted || warp || !strcmp(argv[1], "baseline"), "invalid mode");
      if (hosted) SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE", "1");
      ComPtr<IDXGIFactory1> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
      ComPtr<IDXGIAdapter1> adapter;
      for (UINT i=0; !warp; ++i) {
         ComPtr<IDXGIAdapter1> item; HRESULT hr=factory->EnumAdapters1(i, &item);
         if (hr == DXGI_ERROR_NOT_FOUND) break;
         check(hr, "EnumAdapters1"); DXGI_ADAPTER_DESC1 desc={}; check(item->GetDesc1(&desc), "GetDesc1");
         if (desc.VendorId != 0x1002 || desc.DeviceId != 0x13fe) continue;
         require(!adapter, "ambiguous adapter"); adapter=item;
         uint64_t luid=0; memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
         char text[32]; sprintf_s(text, "%016llx", static_cast<unsigned long long>(luid));
         SetEnvironmentVariableA("BC250_D3D_ZINK_LUID", text);
      }
      require(warp || adapter != nullptr, "adapter missing");
      ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
      D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_10_0;
      check(D3D11CreateDevice(adapter.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
         nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
      const char* witness=warp ? "d3d10warp.dll" : (hosted ? "bc250d3d_zink.dll" : "bc250d3d.dll");
      HMODULE module=GetModuleHandleA(witness); require(module != nullptr, "renderer witness missing");
      char path[MAX_PATH]={}; GetModuleFileNameA(module, path, MAX_PATH);
      printf("mode=%s pid=%lu renderer=%s\n", argv[1], GetCurrentProcessId(), path);
      if (hosted) require(GetModuleHandleA("vulkan_radeon.dll") != nullptr, "ICD witness missing");
      {
         auto vsCode=compile("VS", "vs_4_0"), solidCode=compile("Solid", "ps_4_0");
         auto branchCode=compile("Branch", "ps_4_0"), fetchCode=compile("Fetch", "ps_4_0");
         ComPtr<ID3DBlob> disassembly;
         check(D3DDisassemble(branchCode->GetBufferPointer(), branchCode->GetBufferSize(), 0,
            nullptr, &disassembly), "D3DDisassemble");
         const char* asmText=static_cast<const char*>(disassembly->GetBufferPointer());
         require(strstr(asmText, "if_nz") || strstr(asmText, "if_z"), "branch compiled without conditional opcode");
         puts("branch DXBC conditional opcode verified");
         ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> solid, branch, fetch;
         check(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "VS");
         check(dev->CreatePixelShader(solidCode->GetBufferPointer(), solidCode->GetBufferSize(), nullptr, &solid), "Solid");
         check(dev->CreatePixelShader(branchCode->GetBufferPointer(), branchCode->GetBufferSize(), nullptr, &branch), "Branch");
         check(dev->CreatePixelShader(fetchCode->GetBufferPointer(), fetchCode->GetBufferSize(), nullptr, &fetch), "Fetch");
         D3D11_TEXTURE2D_DESC td={}; td.Width=td.Height=64; td.MipLevels=td.ArraySize=1;
         td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1;
         td.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
         ComPtr<ID3D11Texture2D> target, other, staging, depth;
         check(dev->CreateTexture2D(&td, nullptr, &target), "target");
         check(dev->CreateTexture2D(&td, nullptr, &other), "other");
         td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
         check(dev->CreateTexture2D(&td, nullptr, &staging), "staging");
         td.Usage=D3D11_USAGE_DEFAULT; td.CPUAccessFlags=0; td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
         td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;
         check(dev->CreateTexture2D(&td, nullptr, &depth), "depth texture");
         ComPtr<ID3D11RenderTargetView> rtv, otherRtv; ComPtr<ID3D11DepthStencilView> dsv;
         ComPtr<ID3D11ShaderResourceView> srv;
         check(dev->CreateRenderTargetView(target.Get(), nullptr, &rtv), "rtv");
         check(dev->CreateRenderTargetView(other.Get(), nullptr, &otherRtv), "other rtv");
         check(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "dsv");
         check(dev->CreateShaderResourceView(target.Get(), nullptr, &srv), "srv");
         D3D11_BUFFER_DESC bd={}; bd.ByteWidth=32; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
         ComPtr<ID3D11Buffer> cb; check(dev->CreateBuffer(&bd, nullptr, &cb), "cb");
         ctx->VSSetConstantBuffers(0,1,cb.GetAddressOf()); ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf());
         D3D11_RASTERIZER_DESC rd={}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
         ComPtr<ID3D11RasterizerState> raster; check(dev->CreateRasterizerState(&rd, &raster), "raster");
         ctx->RSSetState(raster.Get()); D3D11_VIEWPORT vp={0,0,64,64,0,1}; ctx->RSSetViewports(1,&vp);
         ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx->VSSetShader(vs.Get(),nullptr,0);
         ctx->PSSetShader(solid.Get(),nullptr,0); ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());
         D3D11_DEPTH_STENCIL_DESC ds={}; ds.DepthEnable=TRUE; ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
         ds.DepthFunc=D3D11_COMPARISON_LESS; ds.StencilReadMask=ds.StencilWriteMask=255;
         ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
         ds.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS; ds.BackFace=ds.FrontFace;
         ComPtr<ID3D11DepthStencilState> depthState, replaceState, equalState, disabledState;
         check(dev->CreateDepthStencilState(&ds,&depthState), "depth state");
         ds.StencilEnable=TRUE; ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE; ds.BackFace=ds.FrontFace;
         check(dev->CreateDepthStencilState(&ds,&replaceState), "replace state");
         ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP; ds.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL; ds.BackFace=ds.FrontFace;
         check(dev->CreateDepthStencilState(&ds,&equalState), "equal state");
         ds.DepthEnable=FALSE; ds.StencilEnable=FALSE;
         check(dev->CreateDepthStencilState(&ds,&disabledState), "disabled state");
         D3D11_BLEND_DESC blendDesc={}; auto& b=blendDesc.RenderTarget[0]; b.BlendEnable=TRUE;
         b.SrcBlend=D3D11_BLEND_SRC_ALPHA; b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA; b.BlendOp=D3D11_BLEND_OP_ADD;
         b.SrcBlendAlpha=D3D11_BLEND_ONE; b.DestBlendAlpha=D3D11_BLEND_ZERO; b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
         b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
         ComPtr<ID3D11BlendState> blend; check(dev->CreateBlendState(&blendDesc,&blend), "blend");
         D3D11_QUERY_DESC qd={D3D11_QUERY_EVENT,0}; ComPtr<ID3D11Query> event;
         check(dev->CreateQuery(&qd,&event), "event");
         auto finish=[&]() {
            ctx->End(event.Get()); ctx->Flush(); ULONGLONG deadline=GetTickCount64()+10000;
            for (;;) { BOOL done=FALSE; HRESULT hr=ctx->GetData(event.Get(),&done,sizeof(done),0); check(hr,"GetData");
               if(hr==S_OK && done)break; require(GetTickCount64()<deadline,"GPU timeout"); Sleep(1); }
         };
         auto draw=[&](float red,float green,float blue,float alpha,float z) {
            float data[8]={red,green,blue,alpha,z,0,0,0}; ctx->UpdateSubresource(cb.Get(),0,nullptr,data,0,0); ctx->Draw(3,0);
         };
         auto read=[&](const char* label,ID3D11Texture2D* source,int red,int green,int blue,int alpha,bool split,int tolerance) {
            ctx->CopyResource(staging.Get(),source); finish(); D3D11_MAPPED_SUBRESOURCE m={};
            check(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Map"); unsigned bad=0; uint64_t hash=14695981039346656037ull;
            for(UINT y=0;y<64;++y)for(UINT x=0;x<64;++x) {
               auto pixel=static_cast<const unsigned char*>(m.pData)+y*m.RowPitch+x*4;
               int expected[4]={red,green,blue,alpha}; if(split){expected[0]=x<32?255:0;expected[1]=x<32?0:255;}
               bool mismatch=false; for(UINT c=0;c<4;++c){if(abs(int(pixel[c])-expected[c])>tolerance)mismatch=true;hash=(hash^pixel[c])*1099511628211ull;}
               if(mismatch && bad++==0)printf("first mismatch %u,%u actual=%u,%u,%u,%u\n",x,y,pixel[0],pixel[1],pixel[2],pixel[3]);
            }
            ctx->Unmap(staging.Get(),0); printf("case=%s mismatches=%u/4096 tolerance=%d fnv64=%016llx\n",label,bad,tolerance,static_cast<unsigned long long>(hash));
            require(bad==0,label);
         };
         const float blue[4]={0,0,1,1}; ctx->ClearRenderTargetView(rtv.Get(),blue);
         ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
         ctx->OMSetDepthStencilState(depthState.Get(),0); draw(1,0,0,1,.25f);
         read("depth-near",target.Get(),255,0,0,255,false,0);
         draw(0,1,0,1,.75f); read("depth-reject-far",target.Get(),255,0,0,255,false,0);
         ctx->OMSetDepthStencilState(replaceState.Get(),3); draw(0,0,1,1,.125f);
         read("stencil-replace",target.Get(),0,0,255,255,false,0);
         ctx->OMSetDepthStencilState(equalState.Get(),2); draw(0,1,0,1,.05f);
         read("stencil-reject",target.Get(),0,0,255,255,false,0);
         ctx->OMSetDepthStencilState(equalState.Get(),3); draw(1,1,0,1,.05f);
         read("stencil-accept",target.Get(),255,255,0,255,false,0);
         ctx->OMSetDepthStencilState(disabledState.Get(),0); ctx->ClearRenderTargetView(rtv.Get(),blue);
         ctx->OMSetBlendState(blend.Get(),nullptr,~0u); draw(1,0,0,.5f,.5f);
         read("alpha-blend",target.Get(),128,0,128,128,false,1);
         ctx->OMSetBlendState(nullptr,nullptr,~0u); ctx->PSSetShader(branch.Get(),nullptr,0); draw(1,1,1,1,.5f);
         read("shader-branch",target.Get(),0,0,0,255,true,0);
         ctx->OMSetRenderTargets(1,otherRtv.GetAddressOf(),nullptr); ctx->PSSetShaderResources(0,1,srv.GetAddressOf());
         ctx->PSSetShader(fetch.Get(),nullptr,0); draw(1,1,1,1,.5f);
         read("render-to-texture",other.Get(),0,0,0,255,true,0);

         if (argc == 3) {
            ctx->PSSetShaderResources(0,0,nullptr);
            ID3D11ShaderResourceView* noView=nullptr; ctx->PSSetShaderResources(0,1,&noView);
            ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr); ctx->PSSetShader(solid.Get(),nullptr,0);
            const char vertexText[]="cbuffer P:register(b0){float4 color;float depth;float3 pad;};float4 VS(float2 p:POSITION):SV_Position{return float4(p,depth,1);}";
            ComPtr<ID3DBlob> vertexCode, vertexError;
            check(D3DCompile(vertexText,sizeof(vertexText)-1,"batch-vertices",nullptr,nullptr,"VS","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vertexCode,&vertexError),"batch VS compile");
            ComPtr<ID3D11VertexShader> vertexShader;
            check(dev->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vertexShader),"batch VS");
            D3D11_INPUT_ELEMENT_DESC element={"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
            ComPtr<ID3D11InputLayout> layout;
            check(dev->CreateInputLayout(&element,1,vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),&layout),"batch layout");
            D3D11_BUFFER_DESC dynamicDesc={}; dynamicDesc.ByteWidth=32; dynamicDesc.Usage=D3D11_USAGE_DYNAMIC;
            dynamicDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER; dynamicDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> dynamicCb, dynamicVb, index;
            check(dev->CreateBuffer(&dynamicDesc,nullptr,&dynamicCb),"dynamic CB");
            dynamicDesc.ByteWidth=64; dynamicDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
            check(dev->CreateBuffer(&dynamicDesc,nullptr,&dynamicVb),"dynamic VB");
            const unsigned short indices[]={0,0,0,0,1,2,3,4,5};
            D3D11_BUFFER_DESC indexDesc={}; indexDesc.ByteWidth=sizeof(indices); indexDesc.BindFlags=D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA initial={}; initial.pSysMem=indices;
            check(dev->CreateBuffer(&indexDesc,&initial,&index),"index buffer");
            D3D11_RASTERIZER_DESC scissorDesc=rd; scissorDesc.ScissorEnable=TRUE;
            ComPtr<ID3D11RasterizerState> scissor;
            check(dev->CreateRasterizerState(&scissorDesc,&scissor),"scissor raster");
            auto discard=[&](ID3D11Buffer* buffer,const void* bytes,size_t size) {
               D3D11_MAPPED_SUBRESOURCE mapped={}; check(ctx->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map DISCARD");
               memcpy(mapped.pData,bytes,size); ctx->Unmap(buffer,0);
            };
            for (UINT variant=0;variant<3;++variant) {
               ctx->RSSetState(variant ? raster.Get() : scissor.Get());
               ctx->VSSetShader(variant ? vertexShader.Get() : vs.Get(),nullptr,0);
               ctx->IASetInputLayout(variant ? layout.Get() : nullptr);
               ID3D11Buffer* constants=variant ? dynamicCb.Get() : cb.Get();
               ctx->VSSetConstantBuffers(0,1,&constants); ctx->PSSetConstantBuffers(0,1,&constants);
               UINT stride=8,offset=variant==1 ? 16 : 0;
               ctx->IASetVertexBuffers(0,1,dynamicVb.GetAddressOf(),&stride,&offset);
               ctx->IASetIndexBuffer(index.Get(),DXGI_FORMAT_R16_UINT,0);
               ctx->ClearRenderTargetView(rtv.Get(),blue);
               for (UINT pass=0;pass<4;++pass) for (UINT n=0;n<64;++n) {
                  UINT tile=(n*17+pass*11)%64,x=tile%8,y=tile/8;
                  float constantsData[8]={float(x*32)/255.0f,float(y*32)/255.0f,float(pass*64)/255.0f,1,.5f,0,0,0};
                  if (!variant) {
                     D3D11_RECT rect={LONG(x*8),LONG(y*8),LONG((x+1)*8),LONG((y+1)*8)};
                     ctx->RSSetScissorRects(1,&rect); ctx->UpdateSubresource(cb.Get(),0,nullptr,constantsData,0,0); ctx->Draw(3,0);
                  } else {
                     const float left=float(x)/4.0f-1, right=float(x+1)/4.0f-1;
                     const float top=1-float(y)/4.0f, bottom=1-float(y+1)/4.0f;
                     float vertices[16]={-99,-99,-99,-99,left,top,right,top,left,bottom,left,bottom,right,top,right,bottom};
                     discard(dynamicCb.Get(),constantsData,sizeof(constantsData));
                     discard(dynamicVb.Get(),vertices,sizeof(vertices));
                     if (variant==1) ctx->Draw(6,0); else ctx->DrawIndexed(6,3,2);
                  }
               }
               ctx->CopyResource(staging.Get(),target.Get()); finish();
               D3D11_MAPPED_SUBRESOURCE mapped={};check(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"batch readback");
               unsigned bad=0;uint64_t hash=14695981039346656037ull;
               for(UINT y=0;y<64;++y)for(UINT x=0;x<64;++x) {
                  auto pixel=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
                  const unsigned expected[4]={(x/8)*32,(y/8)*32,192,255}; bool mismatch=false;
                  for(UINT c=0;c<4;++c){if(pixel[c]!=expected[c])mismatch=true;hash=(hash^pixel[c])*1099511628211ull;}
                  if(mismatch && bad++==0)printf("batch first mismatch %u,%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",x,y,pixel[0],pixel[1],pixel[2],pixel[3],expected[0],expected[1],expected[2],expected[3]);
               }
               ctx->Unmap(staging.Get(),0);
               const char* label=variant==0 ? "batched-scissor-cb-update" : (variant==1 ? "batched-vb-cb-discard" : "batched-indexed-discard");
               printf("case=%s draws=256 mismatches=%u/4096 fnv64=%016llx\n",label,bad,static_cast<unsigned long long>(hash));
               require(bad==0,label);
            }
            puts("PASS batched dynamic draw controls: 768 draws, 12288 checked pixels");
         }
         ctx->ClearState(); finish();
      }
      ctx->Flush(); check(dev->GetDeviceRemovedReason(),"device after resource release");
      puts("PASS native graphics state: 8 cases, 32768 checked pixels");
      return 0;
   } catch(const std::exception& e) { printf("FAIL %s\n",e.what()); return 2; }
}
