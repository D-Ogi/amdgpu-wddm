#include <stdio.h>
#include <string.h>
#include "bc250_clock.h"
#include "smu_v11_8_ppsmc.h"
static int checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line%d: %s\n",__LINE__,#x);}}while(0)
struct backend {int held,begin_count,end_count,temp_count,fail_begin,fail_temp,fail_message,temp;
 unsigned count,mhz,vid,target_mhz,target_vid,initial_vid,bad_read,msg[8],param[8],voltage_reads_after_force;};
static int begin(void*context){struct backend*b=context;b->begin_count++;if(b->fail_begin)return -71;CHECK(!b->held);b->held=1;return 0;}
static void end(void*context){struct backend*b=context;CHECK(b->held==1);b->held=0;b->end_count++;}
static int temperature(void*context,int*out){struct backend*b=context;CHECK(b->held==1);b->temp_count++;*out=b->temp;return b->fail_temp?-72:0;}
static int message(void*context,unsigned msg,unsigned param,unsigned*out){
 struct backend*b=context;CHECK(b->held==1);CHECK(b->temp_count==1);CHECK(b->count<8);
 if(b->count>=8)return -73;
 b->msg[b->count]=msg;b->param[b->count]=param;b->count++;
 if(b->fail_message==(int)b->count)return -74;
 if(msg==PPSMC_MSG_RequestGfxclk){
  // Independent operating-point oracle: a clock request must retain at least
  // the target voltage; a raised voltage must already have been read back.
  CHECK(b->vid<=b->target_vid);
  if(b->initial_vid>b->target_vid)CHECK(b->voltage_reads_after_force>0);
  b->mhz=param;
 } else if(msg==PPSMC_MSG_ForceGfxVid){
  if(param>b->vid)CHECK(b->mhz==b->target_mhz); // voltage-down after clock
  b->vid=param;
 } else if(msg==PPSMC_MSG_GetGfxFrequency)*out=b->mhz;
 else if(msg==PPSMC_MSG_GetGfxVid){
  *out=b->vid;
  if(b->count>2)b->voltage_reads_after_force++;
 }else{CHECK(0);return -75;}
 if(b->bad_read==b->count)(*out)++;
 return 0;
}
static struct bc250_clock_io io(struct backend*b){struct bc250_clock_io r={b,begin,end,temperature,message};return r;}
static struct backend setup(unsigned old_mhz,unsigned old_vid,unsigned mhz,unsigned mv){
 struct backend b={0};b.temp=67000;b.mhz=old_mhz;b.vid=old_vid;b.initial_vid=old_vid;b.target_mhz=mhz;b.target_vid=(1550-mv)*160/1000;return b;
}
static void positive(unsigned old_mhz,unsigned old_vid,unsigned mhz,unsigned mv){
 struct backend b=setup(old_mhz,old_vid,mhz,mv);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
 unsigned staged=old_vid>b.target_vid,at=staged?4:2;
 CHECK(bc250_clock_prepare(&c,mhz,mv,&r)==0);CHECK(r.ready==1 && !r.status);
 CHECK(b.begin_count==1 && b.end_count==1 && !b.held && b.temp_count==1);
 CHECK(r.initial_mhz==old_mhz && r.initial_vid==old_vid && r.voltage_staged==staged);
 CHECK(r.messages_attempted==at+4 && r.messages_completed==at+4);
 CHECK(b.msg[0]==PPSMC_MSG_GetGfxFrequency && !b.param[0]);
 CHECK(b.msg[1]==PPSMC_MSG_GetGfxVid && !b.param[1]);
 if(staged){
  CHECK(b.msg[2]==PPSMC_MSG_ForceGfxVid && b.param[2]==b.target_vid);
  CHECK(b.msg[3]==PPSMC_MSG_GetGfxVid && !b.param[3] && r.staged_vid==b.target_vid);
 }
 CHECK(b.msg[at]==PPSMC_MSG_RequestGfxclk && b.param[at]==mhz);
 CHECK(b.msg[at+1]==PPSMC_MSG_ForceGfxVid && b.param[at+1]==b.target_vid);
 CHECK(b.msg[at+2]==PPSMC_MSG_GetGfxFrequency && b.msg[at+3]==PPSMC_MSG_GetGfxVid);
 CHECK(r.observed_mhz==mhz && r.observed_vid==b.target_vid);
 CHECK(r.requested_mhz==mhz && r.requested_mv==mv && r.temperature_mc==67000);
}
int main(void){
 unsigned path,step;
 positive(1000,116,1500,900); // voltage up, then clock up
 positive(1500,104,1000,820); // clock down, then voltage down
 positive(1000,116,1000,820); // repeated startup
 positive(1000,104,1500,900); // already sufficient voltage
 positive(1000,136,1000,820); // voltage-only increase
 positive(1500,116,1000,900); // mixed directions still stages voltage up
 for(path=0;path<2;path++)for(step=1;step<=(path?8u:6u);step++){
  struct backend b=setup(path?1000:1500,path?116:104,path?1500:1000,path?900:820);
  struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.fail_message=(int)step;
  CHECK(bc250_clock_prepare(&c,b.target_mhz,path?900:820,&r)==-74);CHECK(!r.ready);
  CHECK(r.messages_attempted==step && r.messages_completed==step-1);
  CHECK(b.count==step && b.end_count==1 && !b.held);
  if(path && step<=4)CHECK(b.mhz==1000); // no early frequency request
  if(!path && step==4)CHECK(b.mhz==1000 && b.vid==104); // keep completed downclock
 }
 for(step=4;step<=8;step+=2){
  struct backend b=setup(1000,116,1500,900);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  b.bad_read=step==6?7:step;
  CHECK(bc250_clock_prepare(&c,1500,900,&r)==BC250_CLOCK_MISMATCH);CHECK(!r.ready && b.end_count==1 && !b.held);
  if(step==4)CHECK(b.mhz==1000 && b.count==4);
 }
 for(step=0;step<2;step++){
  struct backend b=setup(step?1000:0,step?256:116,1000,820);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_STATE_INVALID);CHECK(b.count==2 && !r.ready && b.end_count==1);
 }
 {struct backend b=setup(1000,116,1000,820);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=85000;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_TOO_HOT);CHECK(!r.ready && !b.count && b.end_count==1);
  b.temp=67000;b.fail_temp=1;CHECK(bc250_clock_prepare(&c,1000,820,&r)==-72);CHECK(!r.ready && !b.count && b.end_count==2);
 }
 {struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.fail_begin=1;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==-71);CHECK(!r.ready && !b.end_count && !b.count && !b.temp_count);
 }
 {struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  CHECK(bc250_clock_prepare(&c,1501,820,&r)==BC250_CLOCK_INVALID);
  CHECK(bc250_clock_prepare(&c,1000,901,&r)==BC250_CLOCK_INVALID);CHECK(!b.begin_count && !b.count && !r.ready);
  c.end=NULL;CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_INVALID);CHECK(!b.begin_count);
 }
 printf("clock policy: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
