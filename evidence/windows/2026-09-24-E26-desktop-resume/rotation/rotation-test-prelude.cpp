#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
using UINT=unsigned;using UINT64=uint64_t;using HRESULT=long;
#define APIENTRY
#define LOG_ENTRYPOINT() ((void)0)
#define S_OK 0
#define E_NOTIMPL -1
#define E_OUTOFMEMORY -2
#define PIPE_TEXTURE_2D 2
#define PIPE_BIND_DEPTH_STENCIL 4
#define MESA_SHADER_STAGES 3
#define PIPE_MAX_SHADER_SAMPLER_VIEWS 4
#define MESA_SHADER_VERTEX 0
#define MESA_SHADER_FRAGMENT 1
#define MESA_SHADER_GEOMETRY 2
#define DebugPrintf(...) ((void)0)
struct pipe_resource {unsigned target=2,last_level=0,bind=0; int id;};
struct pipe_sampler_view {pipe_resource* texture;int descriptor;};
struct pipe_surface {pipe_resource* texture;};
struct Framebuffer {UINT nr_cbufs; pipe_surface cbufs[3];};
struct RenderTargetView {RenderTargetView* next;pipe_surface surface;};
struct ShaderResourceView {ShaderResourceView* next;pipe_sampler_view* handle;};
struct Resource {void* hRTResource;unsigned allocation;UINT64 gpuVa,gpuBytes;bool presentReady;void* cpuMapping;pipe_resource* resource;};
struct pipe_context {
 void (*flush)(pipe_context*,void*,unsigned);
 pipe_sampler_view* (*create_sampler_view)(pipe_context*,pipe_resource*,const pipe_sampler_view*);
 void (*sampler_view_release)(pipe_context*,pipe_sampler_view*);
 void (*set_framebuffer_state)(pipe_context*,Framebuffer*);
 void (*set_sampler_views)(pipe_context*,unsigned,unsigned,unsigned,unsigned,pipe_sampler_view**);
};
struct Device {pipe_context* pipe;RenderTargetView* renderTargetViews;ShaderResourceView* shaderResourceViews;Framebuffer fb;pipe_sampler_view* sampler_views[3][4];};
struct DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES {Device* hDevice;UINT Resources;Resource** pResources;};
static Device* CastDevice(Device* d){return d;}
static Resource* CastResource(Resource* r){return r;}
static void pipe_resource_reference(pipe_resource** a,pipe_resource* b){*a=b;}
static int flushes, framebuffer_updates, sampler_updates;
static void flush(pipe_context*,void*,unsigned){flushes++;}
static pipe_sampler_view* create(pipe_context*,pipe_resource* r,const pipe_sampler_view* d){return new pipe_sampler_view{r,d->descriptor};}
static void release(pipe_context*,pipe_sampler_view* v){delete v;}
static void fb(pipe_context*,Framebuffer*){framebuffer_updates++;}
static void samplers(pipe_context*,unsigned,unsigned,unsigned,unsigned,pipe_sampler_view**){sampler_updates++;}
