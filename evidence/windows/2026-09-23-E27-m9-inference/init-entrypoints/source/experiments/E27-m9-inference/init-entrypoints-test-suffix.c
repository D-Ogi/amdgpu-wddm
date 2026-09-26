
#define CHECK_INIT(Name,Upper,storage) do { \
 NTSTATUS status,expected; \
 memset(&(storage),0xCC,sizeof(storage));calls=badInput=0; \
 status=Name##InitializeHardware(&d,&(storage)); \
 expected=(mode==1||mode==6)?STATUS_DEVICE_NOT_READY:(mode==5?1:((mode==0)?0:STATUS_IO_DEVICE_ERROR)); \
 check(status==expected,#Name " propagates failures and rejects incomplete success"); \
 check(calls==1&&!badInput,#Name " initializes command once through shared core"); \
 check((NTSTATUS)(storage).NtStatus==expected && (storage).Status==(NT_SUCCESS(expected)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED),#Name " report and return agree"); \
 check((storage).WriteCount==19,#Name " partial diagnostic progress retained"); \
} while(0)
#define CHECK_GUARDS(Name,storage) do { \
 calls=0;mode=0;d.GpuStopUnconfirmed=1; \
 check(Name##InitializeHardware(&d,&(storage))==STATUS_DEVICE_HARDWARE_ERROR&&!calls,#Name " quarantined device does no hardware work"); \
 d.GpuStopUnconfirmed=0;irql=1; \
 check(Name##InitializeHardware(&d,&(storage))==STATUS_INVALID_DEVICE_STATE&&!calls,#Name " wrong IRQL does no hardware work"); \
 irql=0; \
 check(Name##InitializeHardware(NULL,&(storage))==STATUS_INVALID_PARAMETER&&!calls,#Name " absent device refuses"); \
 check(Name##InitializeHardware(&d,NULL)==STATUS_INVALID_PARAMETER&&!calls,#Name " absent report refuses"); \
} while(0)
int main(void)
{
 BC250_DEVICE d={0};
 static BC250_ESCAPE_GART gart;static BC250_ESCAPE_PSP psp;
 static BC250_ESCAPE_IH ih;static BC250_ESCAPE_GFX gfx;
 for(mode=0;mode<=6;mode++){
  CHECK_INIT(Gart,GART,gart);CHECK_INIT(Psp,PSP,psp);CHECK_INIT(Ih,IH,ih);CHECK_INIT(Gfx,GFX,gfx);
 }
 mode=7;CHECK_INIT(Psp,PSP,psp);CHECK_INIT(Gfx,GFX,gfx);
 mode=8;CHECK_INIT(Psp,PSP,psp);
 CHECK_GUARDS(Gart,gart);CHECK_GUARDS(Psp,psp);CHECK_GUARDS(Ih,ih);CHECK_GUARDS(Gfx,gfx);
 // Diagnostic entry points remain simple forwarding: supplied reports are not zeroed.
 mode=0;calls=0;gart.WriteCount=77;GartEscape(&d,&gart);
 psp.WriteCount=77;PspEscape(&d,&psp);ih.WriteCount=77;IhEscape(&d,&ih);gfx.WriteCount=77;GfxEscape(&d,&gfx);
 check(calls==4 && badInput==4,"diagnostic wrappers forward caller input unchanged");
 printf("%d checks, %d failures\n",checks,failures);
 return failures?1:0;
}
