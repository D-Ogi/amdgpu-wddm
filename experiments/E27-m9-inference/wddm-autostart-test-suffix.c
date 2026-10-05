
static void reset(void){
 g_FullWddm=layoutOk=vaGate=1;
 allocs=live=failAlloc=geometryFail=vidmmFail=gpuFail=vidmmLive=vidmmStops=gpuCalls=gpuReady=published=0;
}
int main(void){
 BC250_DEVICE d;NTSTATUS s;int i;BC250_ESCAPE base;
 BC250_ESCAPE_GFX*gfx=(BC250_ESCAPE_GFX*)calloc(1,sizeof(*gfx));
 BC250_ESCAPE_IH*ih=(BC250_ESCAPE_IH*)calloc(1,sizeof(*ih));
 for(i=0;i<8;i++){
  reset();memset(&d,0,sizeof(d));d.Post.TargetId=BC250_CHILD_UID;
  switch(i){case 0:g_FullWddm=0;break;case 1:failAlloc=1;break;case 2:failAlloc=2;break;
  case 3:vaGate=0;break;case 4:layoutOk=0;break;case 5:geometryFail=1;break;
  case 6:vidmmFail=1;break;case 7:gpuFail=1;break;}
  s=WddmStart(&d);
  check(i==0?s==0:!NT_SUCCESS(s),"gate/failure status");
  check(!d.Wddm&&!live&&!published&&!vidmmLive&&!d.WddmAperture.bytes,"failure no publication or resource leak");
  check(vidmmStops==(i>=6?1:0),"only initialized VidMm cleaned");
  check(gpuCalls==(i==7?1:0),"hardware attempts only after preparation");
  if(i>=5)check(s==-(i+2),"native failure preserved");
 }
 reset();memset(&d,0,sizeof(d));d.Post.TargetId=BC250_CHILD_UID;
 check(WddmStart(&d)==0&&gpuCalls==1&&published==1&&d.Wddm&&live==1&&vidmmLive,"successful ready adapter");
 if(d.Wddm)ExFreePoolWithTag(d.Wddm,0);VidMmStop();
 for(i=1;i<=18;i++){
  int expected=i==1||i==2||i==4||i==5||i==12||i==13||i==14||i==17;
  memset(&base,0,sizeof(base));base.Command=(ULONG)i;
  check(WddmDiagnosticAllowed(&base,sizeof(base))==expected,"command admission allowlist");
 }
 gfx->Command=BC250_ESCAPE_RUN_GFX;ih->Command=BC250_ESCAPE_RUN_IH;
 for(i=0;i<5;i++){
  gfx->Op=ih->Op=(ULONG)i;
  check(WddmDiagnosticAllowed((BC250_ESCAPE*)gfx,sizeof(*gfx))==(i==3),"only GFX state");
  check(WddmDiagnosticAllowed((BC250_ESCAPE*)ih,sizeof(*ih))==(i==3),"only IH state");
 }
 gfx->Op=ih->Op=3;
 check(!WddmDiagnosticAllowed((BC250_ESCAPE*)gfx,sizeof(*gfx)-1),"truncated GFX refused");
 check(!WddmDiagnosticAllowed((BC250_ESCAPE*)ih,sizeof(*ih)-1),"truncated IH refused");
 free(gfx);free(ih);
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
