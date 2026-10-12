// Synthetic MMIO for the production ModesetPrepareInherited body. No test
// supplies its verdict: the production reader, allow-list and classifier run.
static ULONG pipeRegisters[0x80000/4];
static unsigned pipeReads,pipeWrites,pipeReadFailure,pipeTear;
static long PipeRead(void* context,ULONG offset,ULONG* value)
{
    (void)context;pipeReads++;
    if(pipeReads==pipeReadFailure)return STATUS_ACCESS_DENIED;
    *value=pipeRegisters[offset/4];
    if(pipeTear && pipeReads>BC250_INHERITED_PIPE_READS && offset==BC250_REG_DMU_DSCL0_RECOUT_START)(*value)++;
    return STATUS_SUCCESS;
}
static long PipeWrite(void* context,ULONG offset,ULONG value)
{(void)context;(void)offset;(void)value;pipeWrites++;return STATUS_ACCESS_DENIED;}
static void IoOpen(BC250_DEVICE* device,BC250_DISP_IO* io,ULONG budget)
{
    memset(io,0,sizeof(*io));io->Context=device;io->Read=PipeRead;io->Write=PipeWrite;io->BudgetUs=budget;
}
static void PipeFixture(ULONG sw,ULONG sh,ULONG tw,ULONG th)
{
    memset(pipeRegisters,0,sizeof(pipeRegisters));pipeReads=pipeWrites=pipeReadFailure=pipeTear=0;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION/4]=sw|(sh<<16);
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=tw|(th<<16);
    pipeRegisters[BC250_REG_DMU_DSCL0_MPC_SIZE/4]=tw|(th<<16);
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_MODE/4]=1;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=(ULONG)(((ULONGLONG)sw<<24)/tw);
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO/4]=(ULONG)(((ULONGLONG)sh<<24)/th);
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG/4]=
        Bc250DispField(HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK,8);
    pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=sw-1;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_BOT_SEL/4]=15;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_CONTROL/4]=2;
}
