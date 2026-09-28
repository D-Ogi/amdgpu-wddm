#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "P:/bc-250/scratch/g0-umd-audit-source/src/gallium/auxiliary/util/u_upload_mgr.c"
struct test_buffer {struct pipe_resource base; unsigned char *data;};
static unsigned created, mapped, unmapped, destroyed;
static bool fail_create;
static struct pipe_resource *create_buffer(struct pipe_screen *screen, const struct pipe_resource *desc)
{
 if(fail_create)return NULL;
 struct test_buffer *b=calloc(1,sizeof(*b));if(!b)return NULL;
 b->base=*desc;b->base.screen=screen;pipe_reference_init(&b->base.reference,1);
 b->data=calloc(1,desc->width0);if(!b->data){free(b);return NULL;}created++;return &b->base;
}
static void destroy_buffer(struct pipe_screen *screen,struct pipe_resource *resource)
{
 (void)screen;struct test_buffer *b=(struct test_buffer *)resource;free(b->data);free(b);destroyed++;
}
static void *map_buffer(struct pipe_context *ctx,struct pipe_resource *resource,unsigned level,unsigned usage,const struct pipe_box *box,struct pipe_transfer **out)
{
 (void)ctx;(void)level;*out=calloc(1,sizeof(**out));if(!*out)return NULL;
 (*out)->resource=resource;(*out)->usage=usage;(*out)->box=*box;mapped++;
 return ((struct test_buffer *)resource)->data+box->x;
}
static void unmap_buffer(struct pipe_context *ctx,struct pipe_transfer *transfer)
{(void)ctx;free(transfer);unmapped++;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x);return 1;}} while(0)
int main(void)
{
 struct pipe_screen screen={.caps={.buffer_map_persistent_coherent=true}};struct pipe_context ctx={0};
 screen.resource_create=create_buffer;screen.resource_destroy=destroy_buffer;
 ctx.screen=&screen;ctx.buffer_map=map_buffer;ctx.buffer_unmap=unmap_buffer;ctx.resource_release=u_default_resource_release;
 _putenv_s("BC250_UPLOAD_AUDIT","1");
 struct u_upload_mgr *u=u_upload_create(&ctx,4096,PIPE_BIND_VERTEX_BUFFER,PIPE_USAGE_STREAM,0);CHECK(u && u->bc250_audit);
 unsigned char data[2048];memset(data,0x5a,sizeof(data));struct pipe_resource *out=NULL,*released=NULL;unsigned offset=~0u;
 u_upload_data(u,0,2048,16,data,&offset,&out,&released);CHECK(offset==0 && !released && memcmp(((struct test_buffer *)out)->data,data,2048)==0);
 u_upload_data(u,0,2048,16,data,&offset,&out,&released);CHECK(offset==2048 && !released && mapped==1);
 u_upload_data(u,0,16,16,data,&offset,&out,&released);CHECK(offset==0 && released && mapped==2 && u->bc250_generation==2);pipe_resource_release(&ctx,released);
 void *ptr=NULL;u_upload_alloc(u,0,32,16,&offset,&out,&released,&ptr);CHECK(ptr && offset==16 && !released);
 CHECK(u->bc250_allocations==4 && u->bc250_requested_bytes==4144 && u->bc250_copy_bytes==4112);
 u_upload_destroy(u);
 u=u_upload_create(&ctx,4096,PIPE_BIND_VERTEX_BUFFER,PIPE_USAGE_STREAM,0);CHECK(u);
 fail_create=true;u_upload_alloc(u,0,8192,16,&offset,&out,&released,&ptr);CHECK(!ptr && offset==~0u && u->bc250_allocations==0 && u->bc250_requested_bytes==0);
 fail_create=false;u_upload_destroy(u);
 _putenv_s("BC250_UPLOAD_AUDIT","0");
 u=u_upload_create(&ctx,4096,PIPE_BIND_VERTEX_BUFFER,PIPE_USAGE_STREAM,0);CHECK(u && !u->bc250_audit);
 u_upload_data(u,0,16,16,data,&offset,&out,&released);CHECK(u->bc250_allocations==0 && u->bc250_copy_bytes==0);
 u_upload_destroy(u);
 printf("PASS actual uploader: rollover, copied content, allocated-vs-copied bytes, failure, audit disabled; created=%u mapped=%u unmapped=%u destroyed=%u\n",created,mapped,unmapped,destroyed);
 return 0;
}
