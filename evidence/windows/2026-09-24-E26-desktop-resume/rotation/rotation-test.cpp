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
HRESULT APIENTRY
_RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   LOG_ENTRYPOINT();
   if (args->Resources <= 1) return S_OK;
   Device *device = CastDevice(args->hDevice);
   pipe_context *pipe = device->pipe;
   // Microsoft DXGI_BASE_FUNCTIONS: rotate kernel identities, preserve RT
   // handles. Copying pixels writes into a buffer still scanned by DCN.
   std::vector<Resource> before(args->Resources);
   for (UINT i = 0; i < args->Resources; ++i) {
      Resource *r = CastResource(args->pResources[i]);
      if (!r || !r->presentReady || !r->resource ||
          r->resource->target != PIPE_TEXTURE_2D || r->resource->last_level ||
          (r->resource->bind & PIPE_BIND_DEPTH_STENCIL)) return E_NOTIMPL;
      before[i] = *r;
   }
   auto rotated = [&](pipe_resource *resource) -> pipe_resource * {
      for (UINT i = 0; i < args->Resources; ++i)
         if (resource == before[i].resource)
            return before[(i + 1) % args->Resources].resource;
      return resource;
   };
   pipe->flush(pipe, NULL, 0);
   // Prepare sampler views before publishing any changed resource identity.
   struct ViewChange { ShaderResourceView *view; pipe_sampler_view *next; };
   std::vector<ViewChange> changes;
   for (ShaderResourceView *v = device->shaderResourceViews; v; v = v->next) {
      if (!v->handle || rotated(v->handle->texture) == v->handle->texture) continue;
      pipe_sampler_view *next = pipe->create_sampler_view(pipe,
         rotated(v->handle->texture), v->handle);
      if (!next) {
         for (auto &c : changes) pipe->sampler_view_release(pipe, c.next);
         return E_OUTOFMEMORY;
      }
      changes.push_back({v, next});
   }
   for (UINT i = 0; i < args->Resources; ++i) {
      Resource *r = CastResource(args->pResources[i]);
      const Resource &next = before[(i + 1) % args->Resources];
      r->resource = next.resource;
      r->allocation = next.allocation;
      r->gpuVa = next.gpuVa;
      r->gpuBytes = next.gpuBytes;
      r->cpuMapping = next.cpuMapping;
      r->presentReady = next.presentReady;
      // hRTResource and the logical resource/view descriptors stay in place.
      DebugPrintf("BC250 Rotate slot %u kernel %x -> %x va %llx\n",
                  i, before[i].allocation, r->allocation, r->gpuVa);
   }
   for (RenderTargetView *v = device->renderTargetViews; v; v = v->next)
      pipe_resource_reference(&v->surface.texture, rotated(v->surface.texture));
   for (UINT i = 0; i < device->fb.nr_cbufs; ++i)
      pipe_resource_reference(&device->fb.cbufs[i].texture,
                              rotated(device->fb.cbufs[i].texture));
   // A cached bound framebuffer and bound samplers also retain old storage.
   pipe->set_framebuffer_state(pipe, &device->fb);
   for (auto &c : changes) {
      pipe_sampler_view *old = c.view->handle;
      for (UINT sh = 0; sh < MESA_SHADER_STAGES; ++sh)
         for (UINT i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++i)
            if (device->sampler_views[sh][i] == old)
               device->sampler_views[sh][i] = c.next;
      c.view->handle = c.next;
      pipe->sampler_view_release(pipe, old);
   }
   if (!changes.empty())
      for (auto sh : {MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_GEOMETRY})
         pipe->set_sampler_views(pipe, sh, 0, PIPE_MAX_SHADER_SAMPLER_VIEWS,
                                 0, device->sampler_views[sh]);
   return S_OK;
}
int main(){
 int failures=0;
 for (unsigned n : {2u,3u}) {
  pipe_context pipe={flush,create,release,fb,samplers};
  pipe_resource storage[3]{};Resource resources[3]{};Resource* handles[3];
  RenderTargetView rtv[3]{};ShaderResourceView srv[3]{};
  Device d{};d.pipe=&pipe;d.fb.nr_cbufs=n;
  for(unsigned i=0;i<n;i++) {
   storage[i].id=i;resources[i]={reinterpret_cast<void*>(uintptr_t(100+i)),20+i,200+i,4096,true,reinterpret_cast<void*>(uintptr_t(300+i)),&storage[i]};handles[i]=&resources[i];
   rtv[i]={i+1<n?&rtv[i+1]:nullptr,{&storage[i]}};
   srv[i]={i+1<n?&srv[i+1]:nullptr,new pipe_sampler_view{&storage[i],int(70+i)}};
   d.fb.cbufs[i].texture=&storage[i];d.sampler_views[1][i]=srv[i].handle;
  }
  d.renderTargetViews=rtv;d.shaderResourceViews=srv;
  DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES a{&d,n,handles};
  for(unsigned turn=1;turn<=6;turn++) {
   if(_RotateResourceIdentities(&a)!=S_OK)failures++;
   for(unsigned i=0;i<n;i++) {
    unsigned expected=(i+turn)%n;
    if(resources[i].resource!=&storage[expected] || resources[i].allocation!=20+expected || resources[i].gpuVa!=200+expected || resources[i].cpuMapping!=reinterpret_cast<void*>(uintptr_t(300+expected)) || resources[i].hRTResource!=reinterpret_cast<void*>(uintptr_t(100+i)))failures++;
    if(rtv[i].surface.texture!=&storage[expected] || d.fb.cbufs[i].texture!=&storage[expected] || srv[i].handle->texture!=&storage[expected] || srv[i].handle->descriptor!=70+int(i) || d.sampler_views[1][i]!=srv[i].handle)failures++;
   }
  }
  for(unsigned i=0;i<n;i++)delete srv[i].handle;
 }
 if(flushes!=12||framebuffer_updates!=12||sampler_updates!=36)failures++;
 printf("2/3 buffer identity, stable runtime handles, retained RTV/SRV and bound views: failures=%d\n",failures);
 return failures?1:0;
}
