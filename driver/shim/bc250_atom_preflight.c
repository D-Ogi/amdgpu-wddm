/* PROVENANCE: operand and control-flow grammar from AMD atom.c (MIT), Linux v6.18.
 * This pass adds full control-flow/call-tree validation before the interpreter's
 * first write. The Linux interpreter itself trusts firmware and lacks this pass. */
#include "bc250_atom_preflight.h"
#include <string.h>
#include "atom_opcodes.inc"

typedef struct PARSE {
    const BC250_ATOM_IMAGE *Image;
    BC250_ATOM_PLAN *Plan;
    BC250_ATOM_PLAN_TABLE *Table;
    uint32_t Pc, Start, End;
} PARSE;

static void Fail(PARSE *p, BC250_ATOM_STATUS status)
{
    if (p->Plan->Status == BC250_ATOM_OK) {
        p->Plan->Status=status;
        p->Plan->FailedOffset=p->Start;
    }
}
static unsigned Take(PARSE *p, unsigned n)
{
    unsigned value=0, i;
    if (p->Plan->Status != BC250_ATOM_OK) return 0;
    if (n>4 || p->Pc>p->End || n>p->End-p->Pc) { Fail(p,BC250_ATOM_BOUNDS); return 0; }
    for (i=0;i<n;++i) value|=(unsigned)p->Image->Bytes[p->Pc++]<<(8*i);
    return value;
}
static unsigned Immediate(unsigned attr)
{ unsigned align=(attr>>3)&7u; return align==0?4u:align<4?2u:1u; }
static void Operand(PARSE *p, unsigned kind, unsigned attr)
{
    unsigned value=Take(p,kind==0 || kind==4 ? 2u : kind==5 ? Immediate(attr):1u);
    if (kind==1) {
        if (value>=BC250_ATOM_PARAMS) Fail(p,BC250_ATOM_PARAMETER);
        if (value+1>p->Table->OwnParameters) p->Table->OwnParameters=(uint16_t)(value+1);
    } else if (kind==2) {
        /* atom.c treats the header's WS count as a count of DWORD slots. */
        if (value>=p->Table->Table.WorkspaceDwords && !(value>=0x40 && value<=0x48))
            Fail(p,BC250_ATOM_PARAMETER);
    } else if (kind==3 || kind>=6) {
        /* No scratch framebuffer, PLL-port or MC-port backend in the display executor. */
        Fail(p,BC250_ATOM_IO);
    }
}
static void Queue(PARSE *p, unsigned relative)
{
    unsigned absolute;
    if (p->Plan->Status != BC250_ATOM_OK) return;
    if (relative<6 || relative>=p->Table->Table.Length) { Fail(p,BC250_ATOM_BOUNDS); return; }
    absolute=p->Table->Table.Offset+relative;
    if (p->Plan->Marks[absolute]==2) { Fail(p,BC250_ATOM_OPCODE); return; }
    if (p->Plan->Marks[absolute]) return;
    if (p->Plan->Tail>=BC250_ATOM_INSTRUCTIONS) { Fail(p,BC250_ATOM_BUDGET); return; }
    p->Plan->Marks[absolute]=3;
    p->Plan->Work[p->Plan->Tail++]=absolute;
}
static BC250_ATOM_STATUS ParseTable(const BC250_ATOM_IMAGE *image, unsigned index, BC250_ATOM_PLAN *plan)
{
    BC250_ATOM_PLAN_TABLE *t=&plan->Tables[index];
    PARSE p={image,plan,t,0,0,0};
    unsigned i;
    BC250_ATOM_STATUS status=Bc250AtomGetTable(image,index,1,&t->Table);
    t->Seen=1;
    if (status==BC250_ATOM_NOT_FOUND) return BC250_ATOM_OK; /* AMD absent called tables are no-ops. */
    if (status!=BC250_ATOM_OK) return status;
    if (t->Table.Offset+t->Table.Length>BC250_ATOM_CODE_LIMIT) return BC250_ATOM_BOUNDS;
    p.End=t->Table.Offset+t->Table.Length;
    for (i=t->Table.Offset;i<p.End;++i) if (plan->Marks[i]) return BC250_ATOM_BOUNDS;
    ++plan->TableCount;
    plan->Head=plan->Tail=0;
    Queue(&p,6);
    while (plan->Head<plan->Tail && plan->Status==BC250_ATOM_OK) {
        unsigned opcode,op,arg,attr=0,start,end,noNext=0;
        p.Pc=plan->Work[plan->Head++]; p.Start=start=p.Pc;
        if (++plan->Instructions>BC250_ATOM_INSTRUCTIONS) { Fail(&p,BC250_ATOM_BUDGET); break; }
        opcode=Take(&p,1);
        if (!opcode || opcode>=sizeof(AtomOpcode)/sizeof(AtomOpcode[0])) { Fail(&p,BC250_ATOM_OPCODE); break; }
        op=AtomOpcode[opcode].op; arg=AtomOpcode[opcode].arg;
        switch (op) {
        case AtomOp_move: case AtomOp_and: case AtomOp_or: case AtomOp_mul: case AtomOp_div:
        case AtomOp_add: case AtomOp_sub: case AtomOp_compare: case AtomOp_test: case AtomOp_xor:
        case AtomOp_shl: case AtomOp_shr: case AtomOp_mul32: case AtomOp_div32:
        case AtomOp_mask: case AtomOp_clear: case AtomOp_shift_left: case AtomOp_shift_right:
            attr=Take(&p,1); Operand(&p,arg,attr);
            if (op==AtomOp_mask) (void)Take(&p,Immediate(attr));
            if (op==AtomOp_shift_left || op==AtomOp_shift_right) {
                if (Take(&p,1)>=32) Fail(&p,BC250_ATOM_PARAMETER);
            } else if (op!=AtomOp_clear) Operand(&p,attr&7u,attr);
            break;
        case AtomOp_setport:
            /* Direct MMIO only. Indirect I/O programs/PCI/PIO need a separate checked backend. */
            if (arg || Take(&p,2)) Fail(&p,BC250_ATOM_IO);
            break;
        case AtomOp_setregblock:
            (void)Take(&p,2); break;
        case AtomOp_setdatablock: {
            unsigned data=Take(&p,1);
            BC250_ATOM_TABLE dt;
            if (data && data!=255 && Bc250AtomGetTable(image,data,0,&dt)!=BC250_ATOM_OK)
                Fail(&p,BC250_ATOM_BOUNDS);
            break;
        }
        case AtomOp_jump:
            Queue(&p,Take(&p,2)); noNext=(arg==2); break;
        case AtomOp_switch:
            attr=Take(&p,1); Operand(&p,attr&7u,attr);
            while (plan->Status==BC250_ATOM_OK) {
                unsigned marker=Take(&p,1);
                if (marker==0x5a) {
                    if (Take(&p,1)!=0x5a) Fail(&p,BC250_ATOM_OPCODE);
                    break;
                }
                if (marker!=0x63) { Fail(&p,BC250_ATOM_OPCODE); break; }
                (void)Take(&p,Immediate(attr)); Queue(&p,Take(&p,2));
            }
            break;
        case AtomOp_calltable: {
            unsigned call=Take(&p,1);
            if (call>=image->CommandCount) Fail(&p,BC250_ATOM_BOUNDS);
            else t->Calls[call/8]|=(uint8_t)(1u<<(call%8));
            break;
        }
        case AtomOp_delay: case AtomOp_postcard: case AtomOp_debug:
            (void)Take(&p,1); break;
        case AtomOp_nop: case AtomOp_beep:
            break;
        case AtomOp_eot:
            noNext=1; break;
        case AtomOp_processds: {
            unsigned n=Take(&p,2);
            if (n>p.End-p.Pc) Fail(&p,BC250_ATOM_BOUNDS); else p.Pc+=n;
            break;
        }
        default:
            /* Repeat/save/restore are unimplemented in the imported interpreter, so refuse. */
            Fail(&p,BC250_ATOM_OPCODE); break;
        }
        end=p.Pc;
        if (plan->Status!=BC250_ATOM_OK) break;
        for (i=start+1;i<end;++i) if (plan->Marks[i]) { Fail(&p,BC250_ATOM_OPCODE); break; }
        if (plan->Status!=BC250_ATOM_OK) break;
        plan->Marks[start]=1; plan->Boundary[start]=1;
        for (i=start+1;i<end;++i) plan->Marks[i]=2;
        if (!noNext) Queue(&p,end-t->Table.Offset);
    }
    return plan->Status;
}

static BC250_ATOM_STATUS CallTree(BC250_ATOM_PLAN *plan, unsigned index, unsigned depth)
{
    BC250_ATOM_PLAN_TABLE *t=&plan->Tables[index];
    unsigned i,total=t->OwnParameters,height=1;
    if (t->Colour==1 || depth>BC250_ATOM_DEPTH) return BC250_ATOM_RECURSION;
    if (t->Colour==2) return BC250_ATOM_OK;
    t->Colour=1;
    for (i=0;i<BC250_ATOM_TABLES;++i) if (t->Calls[i/8] & (1u<<(i%8))) {
        unsigned need;
        BC250_ATOM_STATUS status=CallTree(plan,i,depth+1);
        if (status!=BC250_ATOM_OK) return status;
        if ((unsigned)plan->Tables[i].Height+1u>height) height=(unsigned)plan->Tables[i].Height+1u;
        need=plan->Tables[i].TotalParameters + t->Table.ParameterBytes/4u;
        if (need>total) total=need;
    }
    if (height>BC250_ATOM_DEPTH) return BC250_ATOM_RECURSION;
    if (total>BC250_ATOM_PARAMS) return BC250_ATOM_PARAMETER;
    t->Height=(uint8_t)height;
    t->TotalParameters=(uint16_t)total;
    t->Colour=2;
    return BC250_ATOM_OK;
}

BC250_ATOM_STATUS Bc250AtomPreflight(const BC250_ATOM_IMAGE *image, unsigned root, BC250_ATOM_PLAN *plan)
{
    unsigned i,j,pending=1;
    BC250_ATOM_STATUS status;
    if (!plan) return BC250_ATOM_ARGUMENT;
    memset(plan,0,sizeof(*plan));
    if (!image || !image->Bytes || image->CommandCount>BC250_ATOM_TABLES || root>=image->CommandCount)
        return plan->Status=BC250_ATOM_ARGUMENT;
    plan->ImageBytes=image->Bytes; plan->ImageLength=image->Length; plan->Root=root;
    plan->FailedTable=root;
    status=ParseTable(image,root,plan);
    if (status!=BC250_ATOM_OK) return plan->Status=status;
    if (!plan->Tables[root].Table.Length) return plan->Status=BC250_ATOM_NOT_FOUND;
    while (pending) {
        pending=0;
        for (i=0;i<image->CommandCount;++i) if (plan->Tables[i].Seen) {
            for (j=0;j<image->CommandCount;++j) if ((plan->Tables[i].Calls[j/8] & (1u<<(j%8))) && !plan->Tables[j].Seen) {
                plan->FailedTable=j;
                status=ParseTable(image,j,plan);
                if (status!=BC250_ATOM_OK) return plan->Status=status;
                pending=1;
            }
        }
    }
    plan->Status=CallTree(plan,root,1);
    if (plan->Status==BC250_ATOM_OK) plan->FailedTable=plan->FailedOffset=0;
    return plan->Status;
}
