
int main(void)
{
 BC250_DEVICE d={0};BC250_PSP_FIRMWARE* prepared=NULL;BC250_ESCAPE_PSP report;
 unsigned i;int priorReads;NTSTATUS status;
 d.Psp=&d;d.MmioPspEnabled=1;d.VramMcBase=0x100000000ull;d.VramLength=1ull<<30;
 for(i=1;i<=BC250_FILE_COUNT+1;i++) {
  allocations=0;failAllocation=(int)i;reads=hardwareCalls=0;
  check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_INSUFFICIENT_RESOURCES &&
        !prepared&&!live&&!hardwareCalls,"each allocation failure releases all prepared file buffers without hardware");
 }
 failAllocation=0;
 for(i=0;i<BC250_FILE_COUNT;i++){
  failFile=(int)i;
  check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_IO_DEVICE_ERROR &&
        !prepared&&!live&&report.Result==(long)i&&!hardwareCalls,"each missing file preserves index and unwinds prior files");
 }
 failFile=-1;layoutFail=1;
 check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_IO_DEVICE_ERROR&&!prepared&&!live&&!hardwareCalls,
       "invalid firmware layout unwinds complete file set before hardware");
 layoutFail=0;
 check(PspPrepareFirmware(&d,&report,&prepared)==0&&prepared&&live==BC250_FILE_COUNT+1&&
       report.StagingUsed==17&&!hardwareCalls,"successful preparation owns one stable validated file snapshot");
 priorReads=reads;diskVersion=99;
 status=PspInitializePrepared(&d,prepared,&report);
 check(status==0&&usedByte==17&&reads==priorReads&&live==BC250_FILE_COUNT+1,
       "load borrows original bytes despite changed disk and never reopens or frees files");
 loadFail=1;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_IO_DEVICE_ERROR&&report.CommandsDone==1&&
       reads==priorReads&&live==BC250_FILE_COUNT+1,"partial hardware load retains firmware for caller-owned unwind");
 loadFail=0;hardwareCalls=0;d.VramMcBase++;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_INVALID_DEVICE_STATE&&!hardwareCalls&&reads==priorReads,
       "prepared firmware cannot load against changed VRAM geometry");
 d.VramMcBase--;prepared->Owner=NULL;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_INVALID_DEVICE_STATE&&!hardwareCalls,
       "prepared firmware belongs to one device");
 prepared->Owner=&d;d.GpuStopUnconfirmed=1;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_DEVICE_HARDWARE_ERROR&&!hardwareCalls,
       "quarantine blocks prepared load");
 d.GpuStopUnconfirmed=0;
 PspReleaseFirmware(prepared);prepared=NULL;
 check(!live,"explicit release frees prepared owner and every file once");
 PspReleaseFirmware(NULL);check(!live,"empty release is harmless");
 irql=1;priorReads=reads;
 check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_INVALID_DEVICE_STATE&&!prepared&&reads==priorReads&&!live,
       "preflight refuses file IO above PASSIVE_LEVEL");
 irql=0;
 check(PspPrepareFirmware(NULL,&report,&prepared)==STATUS_INVALID_PARAMETER&&!prepared&&!live,"missing device refuses preparation");
 check(PspPrepareFirmware(&d,NULL,&prepared)==STATUS_INVALID_PARAMETER&&!prepared&&!live,"missing report refuses preparation");
 check(PspPrepareFirmware(&d,&report,NULL)==STATUS_INVALID_PARAMETER&&!live,"missing ownership output refuses preparation");
 check(PspInitializePrepared(&d,NULL,&report)==STATUS_INVALID_PARAMETER&&!hardwareCalls,"missing prepared data cannot fall back to rereading files");
 // Diagnostic path still owns and frees its own file reads.
 PspExecutePrepared(&d,&report,NULL);
 check(!live && usedByte==99 && reads==priorReads+BC250_FILE_COUNT,"legacy execution reads current files and releases its private copy");
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
