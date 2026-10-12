/* PROVENANCE: AMD ATOM interpreter (MIT), Linux v6.18 7d0a66e4, checked extraction in atom_interpreter.inc.
 * Host/KMD code supplies only named register access and a serialized owner. No BIOS writes or allocations here. */
#include "bc250_atom.h"
#include <string.h>
#if defined(BC250_SHIM_KERNEL)
#ifndef true
#define true 1
#define false 0
#endif
#else
#include <stdbool.h>
#endif

typedef uint32_t u32;
struct atom_context;
struct card_info {
    struct atom_context *owner;
    void (*reg_write)(struct card_info *,u32,u32);
    u32 (*reg_read)(struct card_info *,u32);
    void (*mc_write)(struct card_info *,u32,u32);
    u32 (*mc_read)(struct card_info *,u32);
    void (*pll_write)(struct card_info *,u32,u32);
    u32 (*pll_read)(struct card_info *,u32);
};
struct atom_context {
    struct card_info *card;
    uint32_t cmd_table,data_table;
    uint16_t iio[256];
    uint16_t data_block,io_attr,reg_block;
    uint32_t fb_base,divmul[2];
    uint8_t shift;
    int cs_equal,cs_above,io_mode;
    uint32_t *scratch;
    int scratch_size_bytes;
    const BC250_ATOM_IMAGE *image;
    const BC250_ATOM_PLAN *plan;
    const BC250_ATOM_CALLBACKS *io;
    BC250_ATOM_LIMITS limits;
    BC250_ATOM_RESULT result;
    BC250_ATOM_STATUS status;
    uint32_t depth,indirect;
    int hasIndirect;
    uint64_t started;
    uint32_t parameters[BC250_ATOM_PARAMS];
    uint32_t workspaces[BC250_ATOM_DEPTH][256];
    struct card_info callbacks;
};
typedef struct {
    struct atom_context *ctx;
    uint32_t *ps,*ws;
    int ps_size,ws_size,ps_shift;
    uint16_t start;
    unsigned last_jump;
    unsigned long last_jump_jiffies;
    unsigned char abort;
} atom_exec_context;

#include "atom_constants.inc"
#define ATOM_TABLE_NAMES_CNT 0
#define ATOM_IO_NAMES_CNT 0

static void atom_fail(struct atom_context *ctx, BC250_ATOM_STATUS status)
{ if (ctx->status==BC250_ATOM_OK) ctx->status=status; }
static unsigned atom_read(struct atom_context *ctx, int offset, unsigned count)
{
    unsigned i,result=0;
    if (ctx->status!=BC250_ATOM_OK) return 0;
    if (offset<0 || (unsigned)offset>ctx->image->Length || count>ctx->image->Length-(unsigned)offset) {
        atom_fail(ctx,BC250_ATOM_BOUNDS); return 0;
    }
    for (i=0;i<count;++i) result|=(unsigned)ctx->image->Bytes[(unsigned)offset+i]<<(8*i);
    return result;
}
static uint8_t atom_shift(struct atom_context *ctx, unsigned value)
{
    if (value>=32) { atom_fail(ctx,BC250_ATOM_PARAMETER); return 0; }
    return (uint8_t)value;
}
static int atom_check(struct atom_context *ctx)
{
    uint64_t now;
    if (ctx->status!=BC250_ATOM_OK) return 0;
    if (!ctx->io->Owned(ctx->io->Context)) { atom_fail(ctx,BC250_ATOM_CANCELLED); return 0; }
    now=ctx->io->NowUs(ctx->io->Context);
    if (now<ctx->started || now-ctx->started>=ctx->limits.ElapsedUs) { atom_fail(ctx,BC250_ATOM_BUDGET); return 0; }
    return 1;
}
static void atom_delay(struct atom_context *ctx, unsigned us)
{
    if (!atom_check(ctx)) return;
    if (us>ctx->limits.DelayUs-ctx->result.DelayUs) { atom_fail(ctx,BC250_ATOM_BUDGET); return; }
    ctx->result.DelayUs+=us;
    if (ctx->io->DelayUs(ctx->io->Context,us)) atom_fail(ctx,BC250_ATOM_IO);
    (void)atom_check(ctx);
}
static unsigned atom_register(struct atom_context *ctx, unsigned reg)
{
    if (reg==1) {
        if (!ctx->hasIndirect) atom_fail(ctx,BC250_ATOM_IO);
        reg=ctx->indirect/4;
        /* The alias cannot resolve to either index/data register. */
        if (reg<2) atom_fail(ctx,BC250_ATOM_IO);
    }
    return reg;
}
static uint32_t atom_reg_read(struct card_info *card, uint32_t reg)
{
    struct atom_context *ctx=card->owner;
    unsigned value=0;
    if (!atom_check(ctx)) return 0;
    if (!reg) return ctx->indirect;
    reg=atom_register(ctx,reg); ctx->result.Register=reg;
    if (ctx->status!=BC250_ATOM_OK) return 0;
    if (!ctx->io->Allowed(ctx->io->Context,reg,0)) { atom_fail(ctx,BC250_ATOM_IO); return 0; }
    ++ctx->result.Reads;
    if (ctx->io->Read(ctx->io->Context,reg,&value)) atom_fail(ctx,BC250_ATOM_IO);
    return value;
}
static void atom_reg_write(struct card_info *card, uint32_t reg, uint32_t value)
{
    struct atom_context *ctx=card->owner;
    if (!atom_check(ctx)) return;
    /* MM_INDEX/MM_DATA are virtualized; never expose a blanket physical indirection window.
     * atom.c shifts the index value by two. Resolve the resulting DWORD through the same ASIC policy. */
    if (!reg) {
        if (value&3u) { atom_fail(ctx,BC250_ATOM_IO); return; }
        ctx->indirect=value; ctx->hasIndirect=1; return;
    }
    reg=atom_register(ctx,reg); ctx->result.Register=reg;
    if (ctx->status!=BC250_ATOM_OK) return;
    if (!ctx->io->Allowed(ctx->io->Context,reg,1)) { atom_fail(ctx,BC250_ATOM_IO); return; }
    ++ctx->result.Writes; /* Includes a write whose transport returns failure after a possible effect. */
    if (ctx->io->Write(ctx->io->Context,reg,value)) atom_fail(ctx,BC250_ATOM_IO);
}
static uint32_t atom_no_read(struct card_info *card, uint32_t reg)
{ (void)reg; atom_fail(card->owner,BC250_ATOM_IO); return 0; }
static void atom_no_write(struct card_info *card, uint32_t reg, uint32_t value)
{ (void)reg; (void)value; atom_fail(card->owner,BC250_ATOM_IO); }
static uint32_t atom_iio_execute(struct atom_context *ctx,int base,uint32_t index,uint32_t data)
{ (void)base; (void)index; (void)data; atom_fail(ctx,BC250_ATOM_IO); return 0; }
static int amdgpu_atom_execute_table_locked(struct atom_context *,int,uint32_t *,int);

#define U8(p) ((uint8_t)atom_read(ctx->ctx,(p),1))
#define U16(p) ((uint16_t)atom_read(ctx->ctx,(p),2))
#define U32(p) atom_read(ctx->ctx,(p),4)
#define DEBUG(...) do {} while (0)
#define SDEBUG(...) do {} while (0)
#define pr_info(...) atom_fail(ctx->ctx,BC250_ATOM_PARAMETER)
#define DRM_ERROR(...) atom_fail(ctx->ctx,BC250_ATOM_PARAMETER)
#define printk(...) ((void)0)
#define get_unaligned_le32(p) (*(p))
#define cpu_to_le32(v) (v)
#define lower_32_bits(v) ((uint32_t)(v))
#define upper_32_bits(v) ((uint32_t)((v)>>32))
#define do_div(v,d) ((v)/=(d))
#define udelay(n) atom_delay(ctx->ctx,(n))
#define mdelay(n) atom_delay(ctx->ctx,(n)*1000u)
#define msleep(n) atom_delay(ctx->ctx,(n)*1000u)
#define drm_can_sleep() 1
#define jiffies ((unsigned long)ctx->ctx->io->NowUs(ctx->ctx->io->Context))
#define time_after(a,b) ((long)((b)-(a))<0)
#define jiffies_to_msecs(v) ((v)/1000u)

/* Warnings caused by unchanged upstream signatures and no-op Linux logging are limited to this extract. */
#pragma warning(push)
#pragma warning(disable: 4100 4101 4189 4127 4244 4245 4018 4389)
#include "atom_interpreter.inc"
#pragma warning(pop)

static int amdgpu_atom_execute_table_locked(struct atom_context *ctx,int index,uint32_t *params,int words)
{
    const BC250_ATOM_PLAN_TABLE *t;
    atom_exec_context ectx;
    unsigned op,pc;
    if (!atom_check(ctx)) return -1;
    if (index<0 || (unsigned)index>=ctx->image->CommandCount || !ctx->plan->Tables[index].Seen) {
        atom_fail(ctx,BC250_ATOM_BOUNDS); return -1;
    }
    t=&ctx->plan->Tables[index];
    if (!t->Table.Length) return 0;
    if (ctx->depth>=BC250_ATOM_DEPTH) { atom_fail(ctx,BC250_ATOM_RECURSION); return -1; }
    if (words<0 || (unsigned)words<t->TotalParameters || (unsigned)words<t->Table.ParameterBytes/4u) {
        atom_fail(ctx,BC250_ATOM_PARAMETER); return -1;
    }
    memset(&ectx,0,sizeof(ectx));
    ectx.ctx=ctx; ectx.ps=params; ectx.ps_size=words; ectx.ps_shift=t->Table.ParameterBytes/4;
    ectx.start=(uint16_t)t->Table.Offset;
    ectx.ws=ctx->workspaces[ctx->depth++]; ectx.ws_size=t->Table.WorkspaceDwords;
    memset(ectx.ws,0,sizeof(ctx->workspaces[0]));
    pc=t->Table.Offset+6;
    while (atom_check(ctx)) {
        int next;
        ctx->result.Table=(unsigned)index; ctx->result.Offset=pc;
        if (pc<t->Table.Offset+6 || pc>=t->Table.Offset+t->Table.Length || !ctx->plan->Boundary[pc]) {
            atom_fail(ctx,BC250_ATOM_BOUNDS); break;
        }
        if (ctx->result.Instructions>=ctx->limits.Instructions) { atom_fail(ctx,BC250_ATOM_BUDGET); break; }
        ++ctx->result.Instructions;
        op=atom_read(ctx,(int)pc,1);
        if (!op || op>=ATOM_OP_CNT) { atom_fail(ctx,BC250_ATOM_OPCODE); break; }
        next=(int)pc+1;
        opcode_table[op].func(&ectx,&next,opcode_table[op].arg);
        if (ectx.abort) atom_fail(ctx,BC250_ATOM_PARAMETER);
        if (ctx->status!=BC250_ATOM_OK || op==ATOM_OP_EOT) break;
        if (next<0) { atom_fail(ctx,BC250_ATOM_BOUNDS); break; }
        pc=(unsigned)next;
    }
    --ctx->depth;
    return ctx->status==BC250_ATOM_OK?0:-1;
}

size_t Bc250AtomContextSize(void) { return sizeof(struct atom_context); }
BC250_ATOM_STATUS Bc250AtomExecute(void *workspace,size_t workspaceSize,
    const BC250_ATOM_IMAGE *image,unsigned root,const BC250_ATOM_PLAN *plan,
    const BC250_ATOM_CALLBACKS *io,const BC250_ATOM_LIMITS *limits,
    void *parameters,size_t parameterBytes,BC250_ATOM_RESULT *result)
{
    struct atom_context *ctx=workspace;
    uint64_t end;
    if (!result) return BC250_ATOM_ARGUMENT;
    memset(result,0,sizeof(*result)); result->Status=BC250_ATOM_ARGUMENT;
    if (!ctx || workspaceSize<sizeof(*ctx) || !image || !image->Bytes || !plan || plan->Status!=BC250_ATOM_OK ||
        plan->ImageBytes!=image->Bytes || plan->ImageLength!=image->Length || plan->Root!=root ||
        root>=BC250_ATOM_TABLES || !plan->Tables[root].Seen || !plan->Tables[root].Table.Length ||
        !io || !io->Owned || !io->Allowed || !io->Read || !io->Write || !io->NowUs || !io->DelayUs ||
        !limits || !limits->Instructions || limits->Instructions>1000000u || !limits->ElapsedUs ||
        limits->ElapsedUs>2000000u || limits->DelayUs>limits->ElapsedUs || !parameters || !parameterBytes ||
        parameterBytes>sizeof(ctx->parameters) || parameterBytes%4) return result->Status;
    memset(ctx,0,sizeof(*ctx));
    ctx->image=image; ctx->plan=plan; ctx->io=io; ctx->limits=*limits;
    ctx->cmd_table=image->CommandMaster; ctx->data_table=image->DataMaster;
    ctx->callbacks.owner=ctx; ctx->callbacks.reg_read=atom_reg_read; ctx->callbacks.reg_write=atom_reg_write;
    ctx->callbacks.mc_read=ctx->callbacks.pll_read=atom_no_read;
    ctx->callbacks.mc_write=ctx->callbacks.pll_write=atom_no_write; ctx->card=&ctx->callbacks;
    memcpy(ctx->parameters,parameters,parameterBytes);
    ctx->started=io->NowUs(io->Context);
    (void)amdgpu_atom_execute_table_locked(ctx,(int)root,ctx->parameters,BC250_ATOM_PARAMS);
    (void)atom_check(ctx);
    if (ctx->status==BC250_ATOM_OK) memcpy(parameters,ctx->parameters,parameterBytes);
    end=io->NowUs(io->Context);
    ctx->result.ElapsedUs=end>=ctx->started?end-ctx->started:0;
    ctx->result.Status=ctx->status;
    *result=ctx->result;
    return result->Status;
}

static BC250_ATOM_STATUS BatchCheck(const BC250_ATOM_CALLBACKS *io, uint64_t start,
    const BC250_ATOM_LIMITS *limits, BC250_ATOM_RESULT *result)
{
    uint64_t now=io->NowUs(io->Context);
    result->ElapsedUs=now>=start?now-start:0;
    if (!io->Owned(io->Context)) return BC250_ATOM_CANCELLED;
    if (now<start || result->ElapsedUs>=limits->ElapsedUs) return BC250_ATOM_BUDGET;
    return BC250_ATOM_OK;
}

BC250_ATOM_STATUS Bc250AtomExecuteBatch(void *workspace,size_t workspaceSize,
    const BC250_ATOM_IMAGE *image,BC250_ATOM_PLAN *plan,
    const BC250_ATOM_CALLBACKS *io,const BC250_ATOM_LIMITS *limits,
    const BC250_ATOM_COMMAND *commands,unsigned count,BC250_ATOM_RESULT *result)
{
    unsigned i;
    uint64_t start;
    if (!result) return BC250_ATOM_ARGUMENT;
    memset(result,0,sizeof(*result)); result->Status=BC250_ATOM_ARGUMENT;
    if (!workspace || workspaceSize<Bc250AtomContextSize() || !image || !image->Bytes || !plan ||
        !io || !io->Owned || !io->Allowed || !io->Read || !io->Write || !io->NowUs || !io->DelayUs ||
        !limits || !limits->Instructions || limits->Instructions>1000000u || !limits->ElapsedUs ||
        limits->ElapsedUs>2000000u || limits->DelayUs>limits->ElapsedUs ||
        !commands || !count || count>BC250_ATOM_BATCH_COMMANDS) return result->Status;
    start=io->NowUs(io->Context);
    for (i=0;i<count;++i) {
        result->Table=commands[i].Root;
        result->Status=BatchCheck(io,start,limits,result);
        if (result->Status!=BC250_ATOM_OK) return result->Status;
        if (!commands[i].Parameters || !commands[i].ParameterBytes ||
            commands[i].ParameterBytes>BC250_ATOM_PARAMS*4u || commands[i].ParameterBytes%4u)
            return result->Status=BC250_ATOM_ARGUMENT;
        result->Status=Bc250AtomPreflight(image,commands[i].Root,plan);
        if (result->Status!=BC250_ATOM_OK) {
            result->Table=plan->FailedTable; result->Offset=plan->FailedOffset;
            return result->Status;
        }
    }
    for (i=0;i<count;++i) {
        BC250_ATOM_LIMITS remaining;
        BC250_ATOM_RESULT part;
        result->Table=commands[i].Root;
        result->Status=Bc250AtomPreflight(image,commands[i].Root,plan);
        if (result->Status!=BC250_ATOM_OK) return result->Status;
        result->Status=BatchCheck(io,start,limits,result);
        if (result->Status!=BC250_ATOM_OK) return result->Status;
        if (result->Instructions>=limits->Instructions) return result->Status=BC250_ATOM_BUDGET;
        remaining.Instructions=limits->Instructions-result->Instructions;
        remaining.DelayUs=limits->DelayUs-result->DelayUs;
        remaining.ElapsedUs=limits->ElapsedUs-(uint32_t)result->ElapsedUs;
        if (remaining.DelayUs>remaining.ElapsedUs) remaining.DelayUs=remaining.ElapsedUs;
        result->Status=Bc250AtomExecute(workspace,workspaceSize,image,commands[i].Root,plan,io,&remaining,
            commands[i].Parameters,commands[i].ParameterBytes,&part);
        result->Table=part.Table; result->Offset=part.Offset; result->Register=part.Register;
        result->Writes+=part.Writes; result->Reads+=part.Reads;
        result->Instructions+=part.Instructions; result->DelayUs+=part.DelayUs;
        {
            BC250_ATOM_STATUS timeStatus=BatchCheck(io,start,limits,result);
            if (result->Status==BC250_ATOM_OK) result->Status=timeStatus;
        }
        if (result->Status!=BC250_ATOM_OK) return result->Status;
    }
    return result->Status;
}
