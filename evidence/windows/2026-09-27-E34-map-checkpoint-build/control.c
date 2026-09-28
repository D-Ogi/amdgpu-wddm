#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "util/u_atomic.h"
#include "util/simple_mtx.h"
#include <errno.h>
#include <windows.h>
static const char *debug_get_option_bc250_audit_marker(void) { return "control-marker.txt"; }
static bool enabled;
static bool debug_get_option_bc250_map_lifetime(void) { return enabled; }
static uint64_t ticks;
static uint64_t os_time_get_nano(void) { return p_atomic_inc_return(&ticks); }
struct pipe_resource { unsigned target,width0,height0,depth0,format,bind; };
struct pipe_box { int x,y,z,width,height,depth; };
struct zink_resource_object { uint64_t bc250_audit_id; bool bc250_runtime; };
struct zink_resource { struct { struct pipe_resource b; bool is_user_ptr; } base; uint64_t bc250_audit_id; struct zink_resource_object *obj; };
struct zink_context { bool bc250_audit; };
struct zink_transfer { struct { struct { unsigned stride; uintptr_t layer_stride; } b; } base; uint64_t bc250_audit_map_id; struct pipe_resource *staging_res; };
static struct zink_resource *zink_resource(struct pipe_resource *p) { return (struct zink_resource *)p; }
static uint64_t bc250_audit_next_id;
static simple_mtx_t bc250_audit_lifetime_lock = SIMPLE_MTX_INITIALIZER;
static uint64_t bc250_audit_event_sequence;
static uint64_t bc250_audit_requests, bc250_audit_successful, bc250_audit_failed;
static uint64_t bc250_audit_ended, bc250_audit_last_marker;


static uint64_t
bc250_audit_id(void)
{
   if (!debug_get_option_bc250_map_lifetime())
      return 0;
   uint64_t id = p_atomic_inc_return(&bc250_audit_next_id);
   if (!id) {
      fprintf(stderr, "BC250 audit lifetime event=invalid reason=id_wrap\n");
      abort();
   }
   return id;
}




static uint64_t
bc250_audit_map_begin(struct zink_context *ctx, struct pipe_resource *pres,
                      unsigned level, unsigned usage, const struct pipe_box *box)
{
   if (!ctx->bc250_audit)
      return 0;
   struct zink_resource *res = zink_resource(pres);
   uint64_t id = bc250_audit_id();
   simple_mtx_lock(&bc250_audit_lifetime_lock);
   bc250_audit_requests++;
   fprintf(stderr, "BC250 audit lifetime event=begin seq=%llu map=%llu time_ns=%llu ctx=%p resource=%p resource_id=%llu object_id=%llu target=%u width=%u height=%u depth=%u format=%u bind=%x level=%u usage=%x user_ptr=%u runtime=%u x=%d y=%d z=%d box_width=%d box_height=%d box_depth=%d\n",
           (unsigned long long)++bc250_audit_event_sequence,
           (unsigned long long)id, (unsigned long long)os_time_get_nano(), (void *)ctx, (void *)pres,
           (unsigned long long)res->bc250_audit_id, (unsigned long long)res->obj->bc250_audit_id,
           pres->target, pres->width0, pres->height0, pres->depth0, pres->format, pres->bind,
           level, usage, res->base.is_user_ptr, res->obj->bc250_runtime,
           box->x, box->y, box->z, box->width, box->height, box->depth);
   simple_mtx_unlock(&bc250_audit_lifetime_lock);
   return id;
}

static void
bc250_audit_map_result(uint64_t id, struct zink_transfer *trans,
                       struct zink_resource *mapped, void *ptr, unsigned usage)
{
   if (!id)
      return;
   if (ptr)
      trans->bc250_audit_map_id = id;
   simple_mtx_lock(&bc250_audit_lifetime_lock);
   if (ptr)
      bc250_audit_successful++;
   else
      bc250_audit_failed++;
   fprintf(stderr, "BC250 audit lifetime event=result seq=%llu map=%llu time_ns=%llu success=%u resource=%p resource_id=%llu object_id=%llu usage=%x staging=%u stride=%u layer_stride=%llu\n",
           (unsigned long long)++bc250_audit_event_sequence,
           (unsigned long long)id, (unsigned long long)os_time_get_nano(), ptr != NULL,
           ptr ? (void *)&mapped->base.b : NULL,
           (unsigned long long)(ptr ? mapped->bc250_audit_id : 0),
           (unsigned long long)(ptr ? mapped->obj->bc250_audit_id : 0), usage,
           trans && trans->staging_res != NULL,
           trans ? trans->base.b.stride : 0,
           (unsigned long long)(trans ? trans->base.b.layer_stride : 0));
   simple_mtx_unlock(&bc250_audit_lifetime_lock);
}

static void
bc250_audit_map_end(struct zink_transfer *trans)
{
   if (!trans->bc250_audit_map_id)
      return;
   simple_mtx_lock(&bc250_audit_lifetime_lock);
   bc250_audit_ended++;
   fprintf(stderr, "BC250 audit lifetime event=end seq=%llu map=%llu time_ns=%llu\n",
           (unsigned long long)++bc250_audit_event_sequence,
           (unsigned long long)trans->bc250_audit_map_id,
           (unsigned long long)os_time_get_nano());
   simple_mtx_unlock(&bc250_audit_lifetime_lock);
}

static uint64_t
bc250_audit_read_marker(void)
{
   const char *path = debug_get_option_bc250_audit_marker();
   if (!path || !*path)
      return 0;
   FILE *file = fopen(path, "rb");
   if (!file)
      return 0;
   char buffer[32];
   size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
   bool complete = !ferror(file) && feof(file);
   fclose(file);
   if (!complete || !count)
      return 0;
   buffer[count] = 0;
   if (buffer[0] < '0' || buffer[0] > '9')
      return 0;
   errno = 0;
   char *end;
   uint64_t marker = strtoull(buffer, &end, 10);
   if (errno == ERANGE || end == buffer)
      return 0;
   while (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')
      end++;
   return end == buffer + count ? marker : 0;
}

bool
zink_bc250_audit_map_checkpoint(bool sampled)
{
   if (!debug_get_option_bc250_map_lifetime())
      return false;
   uint64_t marker = bc250_audit_read_marker();
   simple_mtx_lock(&bc250_audit_lifetime_lock);
   bool requested = marker > bc250_audit_last_marker;
   if (sampled || requested) {
      if (requested)
         bc250_audit_last_marker = marker;
      fprintf(stderr, "BC250 audit lifetime event=checkpoint seq=%llu time_ns=%llu marker=%llu requests=%llu successful=%llu failed=%llu ended=%llu pending=%llu live=%llu\n",
              (unsigned long long)++bc250_audit_event_sequence,
              (unsigned long long)os_time_get_nano(),
              (unsigned long long)(requested ? marker : 0),
              (unsigned long long)bc250_audit_requests,
              (unsigned long long)bc250_audit_successful,
              (unsigned long long)bc250_audit_failed,
              (unsigned long long)bc250_audit_ended,
              (unsigned long long)(bc250_audit_requests - bc250_audit_successful - bc250_audit_failed),
              (unsigned long long)(bc250_audit_successful - bc250_audit_ended));
      fflush(stderr);
   }
   simple_mtx_unlock(&bc250_audit_lifetime_lock);
   return requested;
}


static void marker(const char *text) {
 FILE *f=fopen("control-marker.txt","wb"); assert(f);
 assert(fwrite(text,1,strlen(text),f)==strlen(text)); assert(fclose(f)==0);
}
static DWORD WINAPI worker(LPVOID arg) {
 (void)arg;
 struct zink_context ctx={true};
 struct zink_resource_object obj={bc250_audit_id(),false};
 struct zink_resource res={0}; res.obj=&obj; res.bc250_audit_id=bc250_audit_id();
 res.base.b=(struct pipe_resource){2,1920,1200,1,105,8};
 struct pipe_box box={0,0,0,1920,1200,1};
 for(unsigned i=0;i<200;i++) {
  struct zink_transfer trans={0};
  uint64_t id=bc250_audit_map_begin(&ctx,&res.base.b,0,2,&box);
  bc250_audit_map_result(id,&trans,&res,(i%2)?&ctx:NULL,2);
  if(i%37==0) assert(!zink_bc250_audit_map_checkpoint(true));
  bc250_audit_map_end(&trans);
 }
 return 0;
}
int main(void) {
 marker("11\n"); assert(!zink_bc250_audit_map_checkpoint(true));
 assert(!bc250_audit_event_sequence);
 enabled=true;
 assert(zink_bc250_audit_map_checkpoint(false));
 assert(!zink_bc250_audit_map_checkpoint(false));
 HANDLE threads[4];
 for(unsigned i=0;i<4;i++){threads[i]=CreateThread(NULL,0,worker,NULL,0,NULL);assert(threads[i]);}
 assert(WaitForMultipleObjects(4,threads,TRUE,60000)==WAIT_OBJECT_0);
 for(unsigned i=0;i<4;i++){DWORD code;assert(GetExitCodeThread(threads[i],&code)&&!code);CloseHandle(threads[i]);}
 marker("12\r\n"); assert(zink_bc250_audit_map_checkpoint(false));
 assert(bc250_audit_requests==800 && bc250_audit_successful==400 && bc250_audit_failed==400 && bc250_audit_ended==400);
 const char *bad[]={"", "-1", "13junk", "0", "18446744073709551616", "11"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){marker(bad[i]);assert(!zink_bc250_audit_map_checkpoint(false));}
 struct zink_context ctx={true};
 struct zink_resource_object obj={bc250_audit_id(),false};
 struct zink_resource res={0};res.obj=&obj;res.bc250_audit_id=bc250_audit_id();
 res.base.b=(struct pipe_resource){0,64,1,1,0,1};
 struct pipe_box box={0,0,0,64,1,1};struct zink_transfer trans={0};
 uint64_t id=bc250_audit_map_begin(&ctx,&res.base.b,0,64,&box);
 bc250_audit_map_result(id,&trans,&res,&ctx,64);
 marker("99");assert(zink_bc250_audit_map_checkpoint(false));
 bc250_audit_map_end(&trans);
 marker("100");assert(zink_bc250_audit_map_checkpoint(false));
 puts("PASS threads=4 requests=801 successes=401 failures=400 ends=401 marker_live=99 marker_closed=100 invalid_markers=6");
 return 0;
}
