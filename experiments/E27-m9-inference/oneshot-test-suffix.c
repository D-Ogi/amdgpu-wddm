
static void reset(ULONG v){
 persisted=cached=v;irql=failOpen=failRead=failWrite=failFlush=opens=closes=writes=flushes=0;
 g_FullWddm=0;
}
int main(void){
 unsigned v,m;
 for(v=0;v<4;v++){
  reset(v);
  check(WddmGateOpen()==(v==1||v==2),"gate selection");
  check(writes==(v==1)&&flushes==(v==1)&&closes==1,"write and flush only oneshot");
  check(persisted==(v==1?0:v),"persistent gate state");
  cached=persisted;
  check(WddmGateOpen()==(v==2),"second load: only explicit persistent mode");
 }
 for(m=0;m<5;m++){
  reset(1);
  if(m==0)irql=1;if(m==1)failOpen=1;if(m==2)failRead=1;
  if(m==3)failWrite=1;if(m==4)failFlush=1;
  check(!WddmGateOpen()&&!g_FullWddm,"failure refuses full table");
  check(persisted==1,"failure never claims durable reset");
  check(closes==(m>=2),"handle closed exactly when opened");
  if(m==3)check(!flushes,"failed write does not flush");
  // Model loss of cached registry writes at reboot: still refuse under fault.
  cached=persisted;check(!WddmGateOpen(),"failure after simulated reboot remains closed");
 }
 reset(1);failWrite=1;
 check(GuardConsumeSetting(L"EnableFullWddm",2)==0,"consume failure cannot fall back to enabled default");
 reset(1);failFlush=1;
 check(GuardConsumeSetting(L"EnableFullWddm",1)==0,"flush failure cannot fall back to oneshot default");
 reset(1);failRead=1;
 check(GuardConsumeSetting(L"missing",7)==7,"ordinary read default preserved");
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
