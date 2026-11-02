// SPDX-License-Identifier: MIT
// BD-208 oracle: the glyph atlas of a Skia text renderer on the installed D3D11 driver, with no window. Chrome's GPU
// process (CEF in the Steam client) draws text from an alpha-only atlas that it fills with many small uploads of glyph
// images; ANGLE's D3D11 back end turns GL_R8 into DXGI_FORMAT_R8_UNORM and GL_ALPHA8 into DXGI_FORMAT_A8_UNORM, and a
// glTexSubImage2D either into UpdateSubresource or into a write map of a staging texture followed by
// CopySubresourceRegion. When the uploads are lost, or the alpha of an A8 texture reads as zero, every text label is
// empty while images and panels are drawn: the BD-208 picture.
// Four cases: {A8_UNORM, R8_UNORM} x {update, staging}. Each frame uploads -glyphs glyph images of odd sizes at tight row
// pitch into the next free cells of a 512x512 atlas (shelf packing; when the atlas is full, packing starts again at
// the top and overwrites old glyphs, as an atlas reuses its plots), and after EVERY upload draws a full-screen pass
// that reads the whole atlas into a R8G8B8A8 target (Load and Sample of the same texel), with no flush in between. At
// the end of each frame the atlas (CopyResource to a staging copy) and the target are read back and compared with a
// CPU model of the atlas. Target texel: R = Load(atlas).a, G = Sample(atlas).a, B = Load(atlas).r; so A8 must give
// (v, v, 0) and R8 (255, 255, v).
// --negative changes the model of the first glyph of frame 1 without an upload: a working probe must then FAIL.
// Output: one JSON file. Exit 0 pass, 1 fail, 2 usage, 3 setup (no adapter, no device, no shader, no output file).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;

namespace {
constexpr unsigned kSize = 512;
const char kShader[] =
    "Texture2D<float4> atlas : register(t0);\n"
    "SamplerState point_clamp : register(s0);\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps(float4 p : SV_Position) : SV_Target {\n"
    "    float4 l = atlas.Load(int3(p.xy, 0));\n"
    "    float4 s = atlas.Sample(point_clamp, p.xy / 512.0);\n"
    "    return float4(l.a, s.a, l.r, 1);\n"
    "}\n";

std::string hex(HRESULT hr) { char b[16]; std::snprintf(b,sizeof(b),"%08X",unsigned(hr)); return b; }
std::string module_path(const wchar_t *name) {
    HMODULE m=GetModuleHandleW(name); if (!m) return "";
    wchar_t w[MAX_PATH]{}; if (!GetModuleFileNameW(m,w,MAX_PATH)) return "";
    char a[MAX_PATH*3]{}; WideCharToMultiByte(CP_UTF8,0,w,-1,a,sizeof(a),nullptr,nullptr);
    std::string s; for (const char *p=a;*p;++p) { if (*p=='\\') s+="\\\\"; else s+=*p; } return s;
}
struct Rng { unsigned s; unsigned next() { s=s*1664525u+1013904223u; return s>>8; } };
struct Mismatch { bool any=false; unsigned frame=0,x=0,y=0,expected=0,actual=0; };
struct Case {
    std::string name; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; bool staging=false;
    HRESULT setup=S_OK,removed=S_OK; unsigned frames=0,glyphs=0,wraps=0;
    unsigned long long atlasBad=0,targetBad=0; Mismatch firstAtlas,firstTarget;
    bool pass=false; std::string why;
};

class Probe {
public:
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11SamplerState> sampler;
    bool negative=false; unsigned frames=24,perFrame=32;

    HRESULT shaders() {
        ComPtr<ID3DBlob> v,p,e;
        HRESULT hr=D3DCompile(kShader,sizeof(kShader)-1,"atlas",nullptr,nullptr,"vs","vs_5_0",0,0,&v,&e);
        if (SUCCEEDED(hr)) hr=D3DCompile(kShader,sizeof(kShader)-1,"atlas",nullptr,nullptr,"ps","ps_5_0",0,0,&p,&e);
        if (SUCCEEDED(hr)) hr=device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs);
        if (SUCCEEDED(hr)) hr=device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps);
        D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
        if (SUCCEEDED(hr)) hr=device->CreateSamplerState(&sd,&sampler);
        return hr;
    }
    static void note(Mismatch &m,unsigned long long &count,unsigned frame,unsigned x,unsigned y,unsigned e,unsigned a) {
        if (!m.any) { m.any=true; m.frame=frame; m.x=x; m.y=y; m.expected=e; m.actual=a; }
        ++count;
    }
    void run(Case &k) {
        D3D11_TEXTURE2D_DESC ad{}; ad.Width=kSize; ad.Height=kSize; ad.MipLevels=1; ad.ArraySize=1; ad.Format=k.format;
        ad.SampleDesc={1,0}; ad.Usage=D3D11_USAGE_DEFAULT; ad.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<unsigned char> zero(size_t(kSize)*kSize,0), model(zero);
        D3D11_SUBRESOURCE_DATA init{zero.data(),kSize,0};
        ComPtr<ID3D11Texture2D> atlas,upload,atlasRead,target,targetRead;
        ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv;
        k.setup=device->CreateTexture2D(&ad,&init,&atlas);
        if (SUCCEEDED(k.setup)) k.setup=device->CreateShaderResourceView(atlas.Get(),nullptr,&srv);
        D3D11_TEXTURE2D_DESC sd=ad; sd.Usage=D3D11_USAGE_STAGING; sd.BindFlags=0;
        if (SUCCEEDED(k.setup) && k.staging) {
            sd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE|D3D11_CPU_ACCESS_READ;
            k.setup=device->CreateTexture2D(&sd,&init,&upload);
        }
        sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(k.setup)) k.setup=device->CreateTexture2D(&sd,nullptr,&atlasRead);
        D3D11_TEXTURE2D_DESC td=ad; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags=D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(k.setup)) k.setup=device->CreateTexture2D(&td,nullptr,&target);
        if (SUCCEEDED(k.setup)) k.setup=device->CreateRenderTargetView(target.Get(),nullptr,&rtv);
        td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(k.setup)) k.setup=device->CreateTexture2D(&td,nullptr,&targetRead);
        if (FAILED(k.setup)) return;

        const bool a8=k.format==DXGI_FORMAT_A8_UNORM;
        D3D11_VIEWPORT vp{0,0,float(kSize),float(kSize),0,1};
        Rng rng{k.staging ? 0x5EEDu : 0xA11Au};
        unsigned x=0,y=0,shelf=0,id=0;
        std::vector<unsigned char> glyph;
        for (unsigned f=0;f<frames;++f) {
            for (unsigned g=0;g<perFrame;++g,++id) {
                const unsigned w=3+rng.next()%21, h=5+rng.next()%23;
                if (x+w>kSize) { x=0; y+=shelf+1; shelf=0; }
                if (y+h>kSize) { x=0; y=0; shelf=0; ++k.wraps; }
                glyph.resize(size_t(w)*h);
                for (unsigned gy=0;gy<h;++gy) for (unsigned gx=0;gx<w;++gx)
                    glyph[size_t(gy)*w+gx]=static_cast<unsigned char>(((gx*31u+gy*17u+id*13u)&0xFFu)|1u);
                for (unsigned gy=0;gy<h;++gy) std::memcpy(&model[size_t(y+gy)*kSize+x],&glyph[size_t(gy)*w],w);
                const bool skip=negative && f==1 && g==0;
                if (skip) for (unsigned gy=0;gy<h;++gy) for (unsigned gx=0;gx<w;++gx) model[size_t(y+gy)*kSize+x+gx]^=0xFFu;
                D3D11_BOX box{x,y,0,x+w,y+h,1};
                if (skip) {
                    // the model changed and the atlas did not: the readback must report it
                } else if (!k.staging) {
                    context->UpdateSubresource(atlas.Get(),0,&box,glyph.data(),w,0);   // tight row pitch, as unpack alignment 1
                } else {
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (FAILED(context->Map(upload.Get(),0,D3D11_MAP_WRITE,0,&m))) { k.why="staging map failed"; return; }
                    for (unsigned gy=0;gy<h;++gy)
                        std::memcpy(static_cast<unsigned char *>(m.pData)+size_t(y+gy)*m.RowPitch+x,&glyph[size_t(gy)*w],w);
                    context->Unmap(upload.Get(),0);
                    context->CopySubresourceRegion(atlas.Get(),0,x,y,0,upload.Get(),0,&box);
                }
                ++k.glyphs;
                x+=w+1; if (h>shelf) shelf=h;
                // one draw after every upload, no flush: the order of the upload and the read is the driver's work
                ID3D11RenderTargetView *rt=rtv.Get(); ID3D11ShaderResourceView *sr=srv.Get(); ID3D11SamplerState *ss=sampler.Get();
                context->OMSetRenderTargets(1,&rt,nullptr); context->RSSetViewports(1,&vp);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); context->IASetInputLayout(nullptr);
                context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
                context->PSSetShaderResources(0,1,&sr); context->PSSetSamplers(0,1,&ss);
                context->Draw(3,0);
            }
            ID3D11ShaderResourceView *none=nullptr; context->PSSetShaderResources(0,1,&none);
            context->CopyResource(atlasRead.Get(),atlas.Get());
            context->CopyResource(targetRead.Get(),target.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(context->Map(atlasRead.Get(),0,D3D11_MAP_READ,0,&m))) { k.why="atlas readback map failed"; return; }
            for (unsigned yy=0;yy<kSize;++yy) {
                const auto *row=static_cast<const unsigned char *>(m.pData)+size_t(yy)*m.RowPitch;
                for (unsigned xx=0;xx<kSize;++xx) {
                    const unsigned e=model[size_t(yy)*kSize+xx];
                    if (row[xx]!=e) note(k.firstAtlas,k.atlasBad,f,xx,yy,e,row[xx]);
                }
            }
            context->Unmap(atlasRead.Get(),0);
            if (FAILED(context->Map(targetRead.Get(),0,D3D11_MAP_READ,0,&m))) { k.why="target readback map failed"; return; }
            for (unsigned yy=0;yy<kSize;++yy) {
                const auto *row=static_cast<const unsigned char *>(m.pData)+size_t(yy)*m.RowPitch;
                for (unsigned xx=0;xx<kSize;++xx) {
                    const unsigned v=model[size_t(yy)*kSize+xx];
                    const unsigned e=a8 ? (v | v<<8) : (0xFFu | 0xFFu<<8 | v<<16);
                    const unsigned a=row[xx*4] | row[xx*4+1]<<8 | row[xx*4+2]<<16;
                    if (a!=e) note(k.firstTarget,k.targetBad,f,xx,yy,e,a);
                }
            }
            context->Unmap(targetRead.Get(),0);
            ++k.frames;
            k.removed=device->GetDeviceRemovedReason();
            if (FAILED(k.removed)) return;
        }
    }
};
void judge(Case &k) {
    if (FAILED(k.setup)) { k.why="setup "+hex(k.setup); return; }
    if (FAILED(k.removed)) { k.why="device removed, reason "+hex(k.removed); return; }
    if (!k.why.empty()) return;
    if (k.atlasBad || k.targetBad) {
        k.why="atlas texels wrong "+std::to_string(k.atlasBad)+", target texels wrong "+std::to_string(k.targetBad); return;
    }
    k.pass=true; k.why="every frame matched the model";
}
void write_mismatch(FILE *f,const char *name,const Mismatch &m) {
    std::fprintf(f,"\"%s\":{\"any\":%s,\"frame\":%u,\"x\":%u,\"y\":%u,\"expected\":\"%06X\",\"actual\":\"%06X\"}",
        name,m.any?"true":"false",m.frame,m.x,m.y,m.expected,m.actual);
}
}

int main(int argc,char **argv) {
    if (argc==2 && !std::strcmp(argv[1],"--help")) {
        std::puts("atlas-text --out result.json [--frames N] [--glyphs N] [--any-adapter] [--negative]"); return 0;
    }
    const char *out=nullptr; bool anyAdapter=false; Probe p;
    for (int i=1;i<argc;++i) {
        if (!std::strcmp(argv[i],"--out") && i+1<argc) out=argv[++i];
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) p.frames=unsigned(std::strtoul(argv[++i],nullptr,10));
        else if (!std::strcmp(argv[i],"--glyphs") && i+1<argc) p.perFrame=unsigned(std::strtoul(argv[++i],nullptr,10));
        else if (!std::strcmp(argv[i],"--any-adapter")) anyAdapter=true;
        else if (!std::strcmp(argv[i],"--negative")) p.negative=true;
        else return 2;
    }
    if (!out || p.frames<2 || p.frames>200 || !p.perFrame || p.perFrame>256) return 2;
    ComPtr<IDXGIFactory1> factory; if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 ad{};
    for (UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> a; if (factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (anyAdapter || (d.VendorId==0x1002 && d.DeviceId==0x13FE)) { adapter=a; ad=d; break; }
    }
    if (!adapter) return 3;
    // The levels ANGLE asks for first; BGRA support as Chrome's compositor needs it.
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    if (FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,4,
        D3D11_SDK_VERSION,&p.device,&got,&p.context))) return 3;
    if (FAILED(p.shaders())) return 3;
    FILE *f=nullptr; if (fopen_s(&f,out,"wb") || !f) return 3;
    std::vector<Case> cases(4);
    cases[0].name="a8-update";  cases[0].format=DXGI_FORMAT_A8_UNORM;
    cases[1].name="a8-staging"; cases[1].format=DXGI_FORMAT_A8_UNORM; cases[1].staging=true;
    cases[2].name="r8-update";  cases[2].format=DXGI_FORMAT_R8_UNORM;
    cases[3].name="r8-staging"; cases[3].format=DXGI_FORMAT_R8_UNORM; cases[3].staging=true;
    bool all=true;
    for (Case &k:cases) { p.run(k); judge(k); all=all && k.pass; }
    char name[256]{}; WideCharToMultiByte(CP_UTF8,0,ad.Description,-1,name,sizeof(name),nullptr,nullptr);
    std::fprintf(f,"{\"tool\":\"atlas-text\",\"pid\":%lu,\"adapter\":\"%s\",\"vendor\":\"%04X\",\"device\":\"%04X\","
        "\"feature_level\":\"%04X\",\"frames\":%u,\"glyphs_per_frame\":%u,\"negative\":%s,\"d3d11_umd\":\"%s\",\"engine\":\"%s\","
        "\"pass\":%s,\"cases\":[",GetCurrentProcessId(),name,ad.VendorId,ad.DeviceId,unsigned(got),p.frames,p.perFrame,
        p.negative?"true":"false",module_path(L"amdgpu_wddm_d3d11.dll").c_str(),module_path(L"amdgpu_wddm_dxvk.dll").c_str(),
        all?"true":"false");
    for (size_t i=0;i<cases.size();++i) {
        const Case &k=cases[i];
        std::fprintf(f,"%s\n  {\"case\":\"%s\",\"setup\":\"%s\",\"removed_reason\":\"%s\",\"frames\":%u,\"glyphs\":%u,\"wraps\":%u,"
            "\"atlas_wrong\":%llu,\"target_wrong\":%llu,",i?",":"",k.name.c_str(),hex(k.setup).c_str(),hex(k.removed).c_str(),
            k.frames,k.glyphs,k.wraps,k.atlasBad,k.targetBad);
        write_mismatch(f,"first_atlas",k.firstAtlas); std::fputc(',',f);
        write_mismatch(f,"first_target",k.firstTarget);
        std::fprintf(f,",\"pass\":%s,\"why\":\"%s\"}",k.pass?"true":"false",k.why.c_str());
    }
    std::fprintf(f,"\n]}\n"); std::fclose(f);
    for (const Case &k:cases) std::printf("%-10s %s %s\n",k.name.c_str(),k.pass?"PASS":"FAIL",k.why.c_str());
    std::printf("ATLAS-TEXT %s\n",all?"PASS":"FAIL");
    return all ? 0 : 1;
}
