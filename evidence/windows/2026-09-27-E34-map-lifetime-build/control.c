#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "util/u_atomic.h"
static bool enabled;
static bool debug_get_option_bc250_map_lifetime(void) { return enabled; }
static uint64_t ticks;
static uint64_t os_time_get_nano(void) { return ++ticks; }
struct pipe_resource { unsigned target,width0,height0,depth0,format,bind; };
struct pipe_box { int x,y,z,width,height,depth; };
struct zink_resource_object { uint64_t bc250_audit_id; bool bc250_runtime; };
struct zink_resource { struct { struct pipe_resource b; bool is_user_ptr; } base; uint64_t bc250_audit_id; struct zink_resource_object *obj; };
struct zink_context { bool bc250_audit; };
struct zink_transfer { struct { struct { unsigned stride; uintptr_t layer_stride; } b; } base; uint64_t bc250_audit_map_id; struct pipe_resource *staging_res; };
static struct zink_resource *zink_resource(struct pipe_resource *p) { return (struct zink_resource *)p; }
static uint64_t bc250_audit_next_id;

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
   fprintf(stderr, "BC250 audit lifetime event=begin map=%llu time_ns=%llu ctx=%p resource=%p resource_id=%llu object_id=%llu target=%u width=%u height=%u depth=%u format=%u bind=%x level=%u usage=%x user_ptr=%u runtime=%u x=%d y=%d z=%d box_width=%d box_height=%d box_depth=%d\n",
           (unsigned long long)id, (unsigned long long)os_time_get_nano(), (void *)ctx, (void *)pres,
           (unsigned long long)res->bc250_audit_id, (unsigned long long)res->obj->bc250_audit_id,
           pres->target, pres->width0, pres->height0, pres->depth0, pres->format, pres->bind,
           level, usage, res->base.is_user_ptr, res->obj->bc250_runtime,
           box->x, box->y, box->z, box->width, box->height, box->depth);
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
   fprintf(stderr, "BC250 audit lifetime event=result map=%llu time_ns=%llu success=%u resource=%p resource_id=%llu object_id=%llu usage=%x staging=%u stride=%u layer_stride=%llu\n",
           (unsigned long long)id, (unsigned long long)os_time_get_nano(), ptr != NULL,
           ptr ? (void *)&mapped->base.b : NULL,
           (unsigned long long)(ptr ? mapped->bc250_audit_id : 0),
           (unsigned long long)(ptr ? mapped->obj->bc250_audit_id : 0), usage,
           trans && trans->staging_res != NULL,
           trans ? trans->base.b.stride : 0,
           (unsigned long long)(trans ? trans->base.b.layer_stride : 0));
}

static void
bc250_audit_map_end(struct zink_transfer *trans)
{
   if (trans->bc250_audit_map_id)
      fprintf(stderr, "BC250 audit lifetime event=end map=%llu time_ns=%llu\n",
              (unsigned long long)trans->bc250_audit_map_id,
              (unsigned long long)os_time_get_nano());
}


int main(void) {
 struct zink_context ctx={0};
 struct zink_resource_object obj={0}, replacement={0};
 struct zink_resource res={0}, staging={0}; res.obj=&obj; staging.obj=&replacement;
 struct zink_transfer trans={0};
 struct pipe_box box={0,0,0,1920,1200,1};
 assert(!bc250_audit_id());
 assert(!bc250_audit_map_begin(&ctx,&res.base.b,0,2,&box));
 bc250_audit_map_result(0,NULL,NULL,NULL,0); bc250_audit_map_end(&trans);
 enabled=true; ctx.bc250_audit=true; bc250_audit_next_id=UINT32_MAX-2ULL;
 res.bc250_audit_id=bc250_audit_id(); obj.bc250_audit_id=bc250_audit_id();
 res.base.b=(struct pipe_resource){2,1920,1200,1,105,8};
 uint64_t first=bc250_audit_map_begin(&ctx,&res.base.b,0,2,&box);
 replacement.bc250_audit_id=bc250_audit_id(); assert(replacement.bc250_audit_id>UINT32_MAX);
 res.obj=&replacement;
 trans.base.b.layer_stride=0x100000001ULL;
 bc250_audit_map_result(first,&trans,&res,&ctx,2);
 assert(trans.bc250_audit_map_id==first); bc250_audit_map_end(&trans);
 trans=(struct zink_transfer){0};
 uint64_t second=bc250_audit_map_begin(&ctx,&res.base.b,0,64,&box);
 staging.bc250_audit_id=bc250_audit_id(); staging.base.b.target=0;
 trans.staging_res=&staging.base.b;
 bc250_audit_map_result(second,&trans,&staging,&ctx,64);
 assert(trans.bc250_audit_map_id==second); bc250_audit_map_end(&trans);
 trans=(struct zink_transfer){0};
 uint64_t failed=bc250_audit_map_begin(&ctx,&res.base.b,0,4,&box);
 bc250_audit_map_result(failed,&trans,&res,NULL,4);
 assert(!trans.bc250_audit_map_id); bc250_audit_map_end(&trans);
 uint64_t oom=bc250_audit_map_begin(&ctx,&res.base.b,0,2,&box);
 bc250_audit_map_result(oom,NULL,NULL,NULL,2);
 puts("PASS audit_off object_replacement staging null_result null_transfer 64bit_ids 64bit_stride context_free_end");
 return 0;
}
