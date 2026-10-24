// Cube subresource diagnostic. No window, no installation, no driver changes.
// Exact positive integer colors are representable in both tested formats.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
template<class T> using Ptr = ComPtr<T>;
static void ck(HRESULT h, const char* where) {
    if (FAILED(h)) { char b[256]; sprintf_s(b, "%s HRESULT=0x%08X", where, unsigned(h)); throw std::runtime_error(b); }
}
static Ptr<ID3DBlob> compile(const std::string& s, const char* entry, const char* target) {
    Ptr<ID3DBlob> b,e;
    HRESULT h=D3DCompile(s.data(),s.size(),"cube-mip",nullptr,nullptr,entry,target,
                        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e);
    if (FAILED(h) && e) fprintf(stderr,"%s\n",(char*)e->GetBufferPointer());
    ck(h,"D3DCompile"); return b;
}
static D3D12_SHADER_BYTECODE code(ID3DBlob* b) { return {b->GetBufferPointer(),b->GetBufferSize()}; }
static D3D12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE t) { D3D12_HEAP_PROPERTIES h={}; h.Type=t; h.CreationNodeMask=h.VisibleNodeMask=1; return h; }
static D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d={}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=1;
    d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags=flags; return d;
}
struct F4 { float v[4]; };
static F4 expected(UINT slice, UINT mip) { return {{float(slice+1),float(mip+1),float(1+(slice*3+mip)%13),1}}; }
static unsigned encodeU(float v,unsigned bits) {
    int e=0; float m=std::frexp(v,&e); return unsigned(e-1+15)<<bits | unsigned(std::lround((m*2-1)*(1u<<bits)));
}
static float decodeU(unsigned v,unsigned bits) {
    unsigned e=v>>bits, m=v&((1u<<bits)-1);
    if(e==31) return m ? NAN : INFINITY;
    return std::ldexp(e ? 1.f+float(m)/(1u<<bits) : float(m)/(1u<<bits), e ? int(e)-15 : -14);
}
static void pack(unsigned char* p, DXGI_FORMAT fmt, F4 f) {
    if(fmt==DXGI_FORMAT_R16G16B16A16_FLOAT) { unsigned short v[4]; for(int c=0;c<4;c++)v[c]=(unsigned short)encodeU(f.v[c],10); memcpy(p,v,8); }
    else { unsigned v=encodeU(f.v[0],6)|(encodeU(f.v[1],6)<<11)|(encodeU(f.v[2],5)<<22); memcpy(p,&v,4); }
}
static F4 unpack(const unsigned char* p,DXGI_FORMAT fmt) {
    F4 f={{0,0,0,1}};
    if(fmt==DXGI_FORMAT_R16G16B16A16_FLOAT) { unsigned short v[4]; memcpy(v,p,8); for(int c=0;c<4;c++) f.v[c]=(v[c]&0x8000 ? -1.f:1.f)*decodeU(v[c]&0x7fff,10); }
    else { unsigned v; memcpy(&v,p,4); f.v[0]=decodeU(v&2047,6); f.v[1]=decodeU((v>>11)&2047,6); f.v[2]=decodeU(v>>22,5); } return f;
}
static bool equal(F4 a,F4 b) { for(int c=0;c<4;c++) if(!std::isfinite(a.v[c])||std::fabs(a.v[c]-b.v[c])>0.0001f)return false; return true; }
struct Context {
    Ptr<ID3D12Device> dev; Ptr<ID3D12CommandQueue> q; Ptr<ID3D12CommandAllocator> alloc;
    Ptr<ID3D12GraphicsCommandList> cmd; Ptr<ID3D12Fence> fence; HANDLE event=nullptr; UINT64 seq=0;
    Ptr<ID3D12DescriptorHeap> views,rtvs; UINT vs=0,rs=0;
    Ptr<ID3D12RootSignature> root; Ptr<ID3D12PipelineState> fill, readers[3], draws[2];
    unsigned checks=0,bad=0,debugBad=0; bool inject=false;
    ~Context(){if(event)CloseHandle(event);}
    Ptr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        auto h=hp(type); auto d=bufferDesc(bytes,flags); Ptr<ID3D12Resource> p;
        ck(dev->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&p)),"buffer"); return p;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i){auto h=views->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*vs;return h;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i){auto h=views->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*vs;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i){auto h=rtvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*rs;return h;}
    void begin(){ck(alloc->Reset(),"allocator reset");ck(cmd->Reset(alloc.Get(),nullptr),"list reset");ID3D12DescriptorHeap* h=views.Get();cmd->SetDescriptorHeaps(1,&h);}
    void barrier(ID3D12Resource* p,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        D3D12_RESOURCE_BARRIER x={};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition={p,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};cmd->ResourceBarrier(1,&x);
    }
    void end(){ck(cmd->Close(),"list close");ID3D12CommandList* l=cmd.Get();q->ExecuteCommandLists(1,&l);
        ck(q->Signal(fence.Get(),++seq),"signal");ck(fence->SetEventOnCompletion(seq,event),"fence event");
        if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU wait exceeded 10 seconds");
        ck(dev->GetDeviceRemovedReason(),"device status");
    }
    void init(const std::string& adapter,bool debug);
    void test(DXGI_FORMAT fmt,UINT slices,bool placed,UINT path);
};
static const char* common=R"(
cbuffer Params : register(b0) { uint baseSlice; uint mip; uint width; uint viewMips; }
float4 color(uint s,uint m){return float4(s+1,m+1,1+(s*3+m)%13,1);}
)";
void Context::init(const std::string& choice,bool debug) {
    if(debug){Ptr<ID3D12Debug>d;ck(D3D12GetDebugInterface(IID_PPV_ARGS(&d)),"debug interface");d->EnableDebugLayer();}
    Ptr<IDXGIFactory6> factory;ck(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"factory");
    Ptr<IDXGIAdapter1> a;
    if(choice=="warp")ck(factory->EnumWarpAdapter(IID_PPV_ARGS(&a)),"WARP");
    else for(UINT i=0;;i++){
        Ptr<IDXGIAdapter1> candidate;HRESULT h=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate));
        if(h==DXGI_ERROR_NOT_FOUND)break;ck(h,"adapter enum");DXGI_ADAPTER_DESC1 d={};candidate->GetDesc1(&d);
        if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&(choice=="default"||d.VendorId==0x10de)){a=candidate;break;}
    }
    if(!a)throw std::runtime_error("requested adapter not found");
    DXGI_ADAPTER_DESC1 ad={};a->GetDesc1(&ad);printf("ADAPTER %ls vendor=%04x device=%04x\n",ad.Description,ad.VendorId,ad.DeviceId);
    ck(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)),"device");
    D3D12_COMMAND_QUEUE_DESC qd={};ck(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");
    ck(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)),"allocator");
    ck(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)),"list");ck(cmd->Close(),"initial close");
    ck(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
    if(!event)throw std::runtime_error("CreateEvent failed");
    D3D12_DESCRIPTOR_HEAP_DESC dh={};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;dh.NumDescriptors=128;dh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ck(dev->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&views)),"view heap");vs=dev->GetDescriptorHandleIncrementSize(dh.Type);
    dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;dh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ck(dev->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&rtvs)),"rtv heap");rs=dev->GetDescriptorHandleIncrementSize(dh.Type);
    D3D12_DESCRIPTOR_RANGE ranges[2]={};for(int i=0;i<2;i++){ranges[i].RangeType=i?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[i].NumDescriptors=1;}
    D3D12_ROOT_PARAMETER rp[3]={};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[0].Constants={0,0,4};
    for(int i=0;i<2;i++){rp[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[i+1].DescriptorTable={1,&ranges[i]};}
    D3D12_STATIC_SAMPLER_DESC samp={};samp.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;samp.AddressU=samp.AddressV=samp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;samp.MaxLOD=D3D12_FLOAT32_MAX;samp.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC rd={};rd.NumParameters=3;rd.pParameters=rp;rd.NumStaticSamplers=1;rd.pStaticSamplers=&samp;
    Ptr<ID3DBlob> blob,err;ck(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err),"root serialize");
    ck(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
    auto cs=compile(std::string(common)+R"(
RWTexture2DArray<float4> dst:register(u0);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
 // For the negative control, the fourth constant names the slice to leave cleared.
 if(viewMips!=0 && baseSlice+p.z==viewMips-1 && mip==3)return;
 if(p.x<width&&p.y<width)dst[p]=color(baseSlice+p.z,mip);
}
)","main","cs_5_1");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd={};cd.pRootSignature=root.Get();cd.CS=code(cs.Get());ck(dev->CreateComputePipelineState(&cd,IID_PPV_ARGS(&fill)),"fill pso");
    for(UINT kind=0;kind<3;kind++){
        std::string source=std::string(common)+(kind==0?"TextureCube<float4> src:register(t0);\n":kind==1?"TextureCubeArray<float4> src:register(t0);\n":"Texture2DArray<float4> src:register(t0);\n");
        source+=R"(
SamplerState pointSampler:register(s0);RWStructuredBuffer<float4> result:register(u0);
float3 direction(uint f,float2 uv){
 if(f==0)return float3(1,-uv.y,-uv.x);if(f==1)return float3(-1,-uv.y,uv.x);
 if(f==2)return float3(uv.x,1,uv.y);if(f==3)return float3(uv.x,-1,-uv.y);
 if(f==4)return float3(uv.x,-uv.y,1);return float3(-uv.x,-uv.y,-1);
}
[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){
 uint face=p.x, level=p.y, probe=p.z;
 float2 uv=probe==0?float2(0,0):float2((probe==1||probe==3)?-.75:.75,probe<=2?-.75:.75);
)";
        if(kind==0)source+="float4 value=src.SampleLevel(pointSampler,direction(face%6,uv),level+baseSlice);\n";
        else if(kind==1)source+="float4 value=src.SampleLevel(pointSampler,float4(direction(face%6,uv),face/6),level+baseSlice);\n";
        else source+="uint size=max(1,width>>level); uint2 pos=uint2(saturate(uv*.5+.5)*size);float4 value=src.Load(int4(pos,face,level));\n";
        source+="result[(face*viewMips+level)*5+probe]=value;}";
        cs=compile(source,"main","cs_5_1");cd.CS=code(cs.Get());ck(dev->CreateComputePipelineState(&cd,IID_PPV_ARGS(&readers[kind])),"read pso");
    }
    auto v=compile("float4 main(uint i:SV_VertexID):SV_Position {return float4(i==2?3:-1,i==1?3:-1,0,1);}","main","vs_5_1");
    auto p=compile(std::string(common)+"float4 main():SV_Target{return color(baseSlice,mip);}","main","ps_5_1");
    for(int f=0;f<2;f++){
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={};pd.pRootSignature=root.Get();pd.VS=code(v.Get());pd.PS=code(p.Get());
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask=~0u;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets=1;pd.RTVFormats[0]=f?DXGI_FORMAT_R11G11B10_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;pd.SampleDesc.Count=1;
        ck(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&draws[f])),"draw pso");
    }
}
void Context::test(DXGI_FORMAT fmt,UINT slices,bool placed,UINT path){
    constexpr UINT mips=7;const UINT count=slices*mips,bpp=fmt==DXGI_FORMAT_R11G11B10_FLOAT?4:8;
    const char* format=bpp==4?"R11G11B10_FLOAT":"RGBA16_FLOAT";const char* writer=path==0?"copy":path==1?"uav":"rtv";
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs={};fs.Format=fmt;ck(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&fs,sizeof(fs)),"format support");
    if(!(fs.Support1&D3D12_FORMAT_SUPPORT1_RENDER_TARGET)||!(fs.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))throw std::runtime_error("required format writer unsupported");
    D3D12_RESOURCE_DESC td={};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=td.Height=64;td.DepthOrArraySize=(UINT16)slices;td.MipLevels=mips;td.Format=fmt;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Ptr<ID3D12Heap> heap;Ptr<ID3D12Resource> tex;
    auto th=hp(D3D12_HEAP_TYPE_DEFAULT);UINT64 placedOffset=0;
    if(placed){auto ai=dev->GetResourceAllocationInfo(0,1,&td);placedOffset=ai.Alignment;
        D3D12_HEAP_DESC hd={};hd.SizeInBytes=ai.SizeInBytes+placedOffset;hd.Alignment=ai.Alignment;hd.Properties=th;hd.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
        ck(dev->CreateHeap(&hd,IID_PPV_ARGS(&heap)),"placed heap");ck(dev->CreatePlacedResource(heap.Get(),placedOffset,&td,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&tex)),"placed texture");
    }else ck(dev->CreateCommittedResource(&th,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&tex)),"committed texture");
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);std::vector<UINT> rows(count);std::vector<UINT64> rowBytes(count);UINT64 bytes=0;
    dev->GetCopyableFootprints(&td,0,count,0,footprints.data(),rows.data(),rowBytes.data(),&bytes);
    auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    unsigned char* mapped=nullptr;D3D12_RANGE none={0,0};ck(upload->Map(0,&none,(void**)&mapped),"upload map");memset(mapped,0xcd,(size_t)bytes);
    for(UINT s=0;s<slices;s++)for(UINT m=0;m<mips;m++){UINT sub=m+s*mips;auto&fp=footprints[sub];for(UINT y=0;y<fp.Footprint.Height;y++)for(UINT x=0;x<fp.Footprint.Width;x++)pack(mapped+fp.Offset+y*fp.Footprint.RowPitch+x*bpp,fmt,expected(s,m));}
    upload->Unmap(0,nullptr);
    begin();if(placed){D3D12_RESOURCE_BARRIER b={};b.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;b.Aliasing.pResourceAfter=tex.Get();cmd->ResourceBarrier(1,&b);}
    auto written=path==0?D3D12_RESOURCE_STATE_COPY_DEST:path==1?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_RENDER_TARGET;
    // Placed RT-capable resources require metadata initialization before UAV/draw.
    // Clear to a value outside the oracle domain, so an omitted writer cannot pass.
    barrier(tex.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
    for(UINT s=0;s<slices;s++)for(UINT m=0;m<mips;m++){
        D3D12_RENDER_TARGET_VIEW_DESC r={};r.Format=fmt;r.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        r.Texture2DArray.MipSlice=m;r.Texture2DArray.FirstArraySlice=s;r.Texture2DArray.ArraySize=1;
        auto h=rtv(m+s*mips);dev->CreateRenderTargetView(tex.Get(),&r,h);float zero[4]={};cmd->ClearRenderTargetView(h,zero,0,nullptr);
    }
    if(written!=D3D12_RESOURCE_STATE_RENDER_TARGET)barrier(tex.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,written);
    if(path==0){for(UINT sub=0;sub<count;sub++){
        if(inject && sub==3+(slices==12?7:4)*mips)continue;
        D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=tex.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=sub;
        D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[sub];cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }}else if(path==1){
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(fill.Get());
        for(UINT m=0;m<mips;m++)for(UINT part=0;part<2;part++){
            // Reverse range order; every mip includes a nonzero FirstArraySlice.
            UINT first=part?0:slices/2,n=slices/2,slot=m*2+part;
            D3D12_UNORDERED_ACCESS_VIEW_DESC u={};u.Format=fmt;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;u.Texture2DArray.MipSlice=m;u.Texture2DArray.FirstArraySlice=first;u.Texture2DArray.ArraySize=n;
            dev->CreateUnorderedAccessView(tex.Get(),nullptr,&u,cpu(slot));UINT values[4]={first,m,64u>>m,inject?(slices==12?8u:5u):0u};cmd->SetComputeRoot32BitConstants(0,4,values,0);cmd->SetComputeRootDescriptorTable(2,gpu(slot));cmd->Dispatch(((64u>>m)+7)/8,((64u>>m)+7)/8,n);
        }
    }else{
        cmd->SetGraphicsRootSignature(root.Get());cmd->SetPipelineState(draws[bpp==4].Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for(UINT s=0;s<slices;s++)for(UINT m=0;m<mips;m++){
            if(inject && s==(slices==12?7u:4u) && m==3)continue;
            UINT sub=m+s*mips,edge=64u>>m;D3D12_RENDER_TARGET_VIEW_DESC r={};r.Format=fmt;r.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;r.Texture2DArray.MipSlice=m;r.Texture2DArray.FirstArraySlice=s;r.Texture2DArray.ArraySize=1;
            auto h=rtv(sub);dev->CreateRenderTargetView(tex.Get(),&r,h);cmd->OMSetRenderTargets(1,&h,FALSE,nullptr);
            D3D12_VIEWPORT vp={0,0,float(edge),float(edge),0,1};D3D12_RECT rect={0,0,LONG(edge),LONG(edge)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&rect);
            UINT values[4]={s,m,edge,0};cmd->SetGraphicsRoot32BitConstants(0,4,values,0);cmd->DrawInstanced(3,1,0,0);
        }
    }
    barrier(tex.Get(),written,D3D12_RESOURCE_STATE_COPY_SOURCE);
    for(UINT sub=0;sub<count;sub++){
        D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprints[sub];
        D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=tex.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=sub;cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    barrier(tex.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);end();
    auto check=[&](const char* view,UINT slice,UINT mip,F4 got,UINT probe,bool& subBad){
        F4 want=expected(slice,mip);
        checks++;if(!equal(got,want)){bad++;if(!subBad)printf("WRONG path=%s format=%s allocation=%s slices=%u view=%s slice=%u mip=%u probe=%u got=%.7g,%.7g,%.7g,%.7g expected=%.7g,%.7g,%.7g,%.7g\n",writer,format,placed?"placed":"committed",slices,view,slice,mip,probe,got.v[0],got.v[1],got.v[2],got.v[3],want.v[0],want.v[1],want.v[2],want.v[3]);subBad=true;}
    };
    D3D12_RANGE readRange={0,(SIZE_T)bytes};ck(readback->Map(0,&readRange,(void**)&mapped),"copy map");UINT badSubs=0;
    for(UINT s=0;s<slices;s++)for(UINT m=0;m<mips;m++){bool sb=false;auto&fp=footprints[m+s*mips];for(UINT y=0;y<fp.Footprint.Height;y++)for(UINT x=0;x<fp.Footprint.Width;x++)check("copy-readback",s,m,unpack(mapped+fp.Offset+y*fp.Footprint.RowPitch+x*bpp,fmt),y*fp.Footprint.Width+x,sb);badSubs+=sb;}
    readback->Unmap(0,&none);printf("TABLE path=%s format=%s allocation=%s slices=%u view=copy-readback wrong=%u/%u offset=%llu\n",writer,format,placed?"placed":"committed",slices,badSubs,count,placedOffset);
    // Cube, full array, mip-window cube, mip-window array, second cube, integer Load.
    struct View{UINT kind,first,faces,md,levels;const char* name;bool clamp=false;};
    std::vector<View> cases={{0,0,6,0,7,"cube"},{1,0,slices,0,7,"cube-array"},{0,0,6,2,3,"cube-mip2"},{1,0,slices,2,3,"cube-array-mip2"},{2,0,slices,0,7,"load"},{2,0,slices,2,3,"load-mip2"}};
    if(slices==12){cases.push_back({1,6,6,0,7,"cube-array-first6"});cases.push_back({1,6,6,2,3,"cube-array-first6-mip2"});cases.push_back({2,6,6,2,3,"load-first6-mip2"});}
    cases.push_back({0,0,6,2,3,"cube-mip2-upper-clamp",true});
    cases.push_back({1,0,slices,2,3,"cube-array-mip2-upper-clamp",true});
    if(slices==12)cases.push_back({1,6,6,2,3,"cube-array-first6-mip2-upper-clamp",true});
    for(auto v:cases){
        D3D12_SHADER_RESOURCE_VIEW_DESC sd={};sd.Format=fmt;sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(v.kind==0){sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;sd.TextureCube.MostDetailedMip=v.md;sd.TextureCube.MipLevels=v.levels;}
        else if(v.kind==1){sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;sd.TextureCubeArray.MostDetailedMip=v.md;sd.TextureCubeArray.MipLevels=v.levels;sd.TextureCubeArray.First2DArrayFace=v.first;sd.TextureCubeArray.NumCubes=v.faces/6;}
        else{sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;sd.Texture2DArray.MostDetailedMip=v.md;sd.Texture2DArray.MipLevels=v.levels;sd.Texture2DArray.FirstArraySlice=v.first;sd.Texture2DArray.ArraySize=v.faces;}
        UINT evalLevels=v.clamp?1:v.levels;
        dev->CreateShaderResourceView(tex.Get(),&sd,cpu(100));UINT elements=v.faces*evalLevels*5;UINT64 size=elements*sizeof(F4);
        auto output=buffer(size,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto rb=buffer(size,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud={};ud.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=elements;ud.Buffer.StructureByteStride=sizeof(F4);dev->CreateUnorderedAccessView(output.Get(),nullptr,&ud,cpu(101));
        begin();cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(readers[v.kind].Get());UINT values[4]={v.clamp?v.levels+4:0u,0,64u>>v.md,evalLevels};cmd->SetComputeRoot32BitConstants(0,4,values,0);cmd->SetComputeRootDescriptorTable(1,gpu(100));cmd->SetComputeRootDescriptorTable(2,gpu(101));cmd->Dispatch(v.faces,evalLevels,5);barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(rb.Get(),0,output.Get(),0,size);end();
        F4* results=nullptr;D3D12_RANGE range={0,(SIZE_T)size};ck(rb->Map(0,&range,(void**)&results),"shader map");badSubs=0;
        for(UINT s=0;s<v.faces;s++)for(UINT m=0;m<evalLevels;m++){bool sb=false;for(UINT p=0;p<5;p++)check(v.name,v.first+s,v.md+(v.clamp?v.levels-1:m),results[(s*evalLevels+m)*5+p],p,sb);badSubs+=sb;}
        rb->Unmap(0,&none);printf("TABLE path=%s format=%s allocation=%s slices=%u view=%s wrong=%u/%u\n",writer,format,placed?"placed":"committed",slices,v.name,badSubs,v.faces*evalLevels);
    }
}
int main(int argc,char**argv){
    setvbuf(stdout,nullptr,_IONBF,0);Context c;std::string adapter="default";bool debug=false;
    for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--adapter"&&i+1<argc)adapter=argv[++i];else if(a=="--inject")c.inject=true;else if(a=="--debug")debug=true;else{fprintf(stderr,"usage: cube-mip.exe [--adapter default|nvidia|warp] [--debug] [--inject]\n");return 1;}}
    if(adapter!="default"&&adapter!="nvidia"&&adapter!="warp")return 1;
    // This process is bounded even if a runtime call fails to return.
    std::thread([]{Sleep(170000);fprintf(stderr,"FAIL watchdog 170 seconds\n");TerminateProcess(GetCurrentProcess(),1);}).detach();
    try{
        c.init(adapter,debug);
        for(auto f:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R11G11B10_FLOAT})for(UINT s:{6u,12u})for(bool placed:{false,true})for(UINT p=0;p<3;p++)c.test(f,s,placed,p);
        if(debug){Ptr<ID3D12InfoQueue> info;ck(c.dev.As(&info),"info queue");for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();i++){
            SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<unsigned char>b(n);auto*m=(D3D12_MESSAGE*)b.data();ck(info->GetMessage(i,m,&n),"debug message");
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){c.debugBad++;printf("DEBUG_ERROR %s\n",m->pDescription);}
        }}
        printf("RESULT %s checks=%u mismatches=%u debug_errors=%u injection=%u\n",c.bad||c.debugBad?"FAIL":"PASS",c.checks,c.bad,c.debugBad,unsigned(c.inject));return c.bad||c.debugBad?1:0;
    }catch(const std::exception&e){fprintf(stderr,"RESULT FAIL setup_or_runtime=%s checks=%u\n",e.what(),c.checks);return 1;}
}
