#include <stdio.h>
#include <string.h>
#include "bc250_clock.h"
#include "smu_v11_8_ppsmc.h"
static int checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line%d: %s\n",__LINE__,#x);}}while(0)
#define MAXMSG 128u
struct backend {int held,begin_count,end_count,temp_count,fail_begin,fail_temp,fail_message,temp,read_delta;
 unsigned count,mhz,vid,target_mhz,target_vid,initial_vid,bad_read,msg[MAXMSG],param[MAXMSG],voltage_reads_after_force;
 unsigned ramp_reads,ramp_left,ramp_from,requested,post_reads,delays;};
static int begin(void*context){struct backend*b=context;b->begin_count++;if(b->fail_begin)return -71;CHECK(!b->held);b->held=1;return 0;}
static void end(void*context){struct backend*b=context;CHECK(b->held==1);b->held=0;b->end_count++;}
static int temperature(void*context,int*out){struct backend*b=context;CHECK(b->held==1);b->temp_count++;*out=b->temp;return b->fail_temp?-72:0;}
static void delay(void*context,unsigned usec){
 struct backend*b=context;CHECK(b->held==1);CHECK(usec==BC250_CLOCK_SETTLE_US);
 CHECK(b->post_reads==b->delays+1); // one pause before each re-read, none before the first read
 b->delays++;
}
// Ramp model: after a clock request the next ramp_reads frequency reads lie strictly between the old clock and the
// requested one, evenly spaced; then the request.
static unsigned ramp_value(struct backend*b){
 unsigned k=b->ramp_reads-b->ramp_left+1,n=b->ramp_reads+1;
 return b->ramp_from<b->mhz ? b->ramp_from+(b->mhz-b->ramp_from)*k/n : b->ramp_from-(b->ramp_from-b->mhz)*k/n;
}
static int message(void*context,unsigned msg,unsigned param,unsigned*out){
 struct backend*b=context;CHECK(b->held==1);CHECK(b->temp_count==1);CHECK(b->count<MAXMSG);
 if(b->count>=MAXMSG)return -73;
 b->msg[b->count]=msg;b->param[b->count]=param;b->count++;
 if(b->fail_message==(int)b->count)return -74;
 if(msg==PPSMC_MSG_RequestGfxclk){
  // Independent operating-point oracle: a clock request must retain at least
  // the target voltage; a raised voltage must already have been read back.
  CHECK(b->vid<=b->target_vid);
  if(b->initial_vid>b->target_vid)CHECK(b->voltage_reads_after_force>0);
  b->ramp_from=b->mhz;b->ramp_left=b->ramp_reads;b->requested=1;
  b->mhz=param;
 } else if(msg==PPSMC_MSG_ForceGfxVid){
  if(param>b->vid)CHECK(b->mhz==b->target_mhz); // voltage-down after clock
  b->vid=param;
 } else if(msg==PPSMC_MSG_GetGfxFrequency){
  if(b->requested)b->post_reads++;
  if(b->ramp_left){*out=ramp_value(b);b->ramp_left--;}
  else *out=b->mhz;
 } else if(msg==PPSMC_MSG_GetGfxVid){
  *out=b->vid;
  if(b->count>2)b->voltage_reads_after_force++;
 }else{CHECK(0);return -75;}
 if(b->bad_read==b->count)*out+=b->read_delta?(unsigned)b->read_delta:1u;
 return 0;
}
static struct bc250_clock_io io(struct backend*b){struct bc250_clock_io r={b,begin,end,temperature,message,delay};return r;}
static struct backend setup(unsigned old_mhz,unsigned old_vid,unsigned mhz,unsigned mv){
 struct backend b;memset(&b,0,sizeof(b));
 b.temp=67000;b.mhz=old_mhz;b.vid=old_vid;b.initial_vid=old_vid;b.target_mhz=mhz;b.target_vid=(1550-mv)*160/1000;return b;
}
static void positive(unsigned old_mhz,unsigned old_vid,unsigned mhz,unsigned mv){
 struct backend b=setup(old_mhz,old_vid,mhz,mv);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
 unsigned staged=old_vid>b.target_vid,at=staged?4:2;
 CHECK(bc250_clock_prepare(&c,mhz,mv,&r)==0);CHECK(r.ready==1 && !r.status);
 CHECK(b.begin_count==1 && b.end_count==1 && !b.held && b.temp_count==1);
 CHECK(r.initial_mhz==old_mhz && r.initial_vid==old_vid && r.voltage_staged==staged);
 CHECK(r.messages_attempted==at+4 && r.messages_completed==at+4);
 CHECK(!r.settle_reads && !b.delays && b.post_reads==1);
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
// The clock reads 'reads' times on its way before it arrives. Within the budget the transaction waits for it; past
// the budget it is MISMATCH with the request left standing (no rollback), the last reading on the way.
static void ramp(unsigned old_mhz,unsigned old_vid,unsigned mhz,unsigned mv,unsigned reads,int with_delay){
 struct backend b=setup(old_mhz,old_vid,mhz,mv);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
 unsigned staged=old_vid>b.target_vid,at=staged?4:2,settled=reads<=BC250_CLOCK_SETTLE_READS,i;
 unsigned n=settled?reads:BC250_CLOCK_SETTLE_READS;
 if(!with_delay)c.delay=NULL;
 b.ramp_reads=reads;
 CHECK(bc250_clock_prepare(&c,mhz,mv,&r)==(settled?0:BC250_CLOCK_MISMATCH));
 CHECK(r.ready==settled && r.settle_reads==n && b.post_reads==n+1 && b.delays==(with_delay?n:0u));
 CHECK(r.messages_attempted==at+4+n && r.messages_completed==r.messages_attempted);
 CHECK(b.msg[at]==PPSMC_MSG_RequestGfxclk && b.msg[at+1]==PPSMC_MSG_ForceGfxVid);
 for(i=0;i<=n;i++)CHECK(b.msg[at+2+i]==PPSMC_MSG_GetGfxFrequency);
 CHECK(b.msg[at+3+n]==PPSMC_MSG_GetGfxVid && r.observed_vid==b.target_vid);
 CHECK(b.mhz==mhz && b.vid==b.target_vid && b.end_count==1 && !b.held);
 if(settled)CHECK(r.observed_mhz==mhz);
 else if(old_mhz<mhz)CHECK(r.observed_mhz>=old_mhz && r.observed_mhz<mhz);
 else CHECK(r.observed_mhz<=old_mhz && r.observed_mhz>mhz);
}
int main(void){
 unsigned path,step;
 positive(1000,116,1500,919); // voltage up, then clock up
 positive(1500,104,1000,820); // clock down, then voltage down
 positive(1000,116,1000,820); // repeated startup
 positive(1000,100,1500,919); // already sufficient voltage
 positive(1000,136,1000,820); // voltage-only increase
 positive(1500,116,1000,900); // mixed directions still stages voltage up
 ramp(1000,116,1200,860,3,1);                        // trial 140's raise: 1029 MHz right after the commit
 ramp(1000,116,1200,860,1,0);                        // no delay callback: re-read at once
 ramp(1000,116,2000,1000,BC250_CLOCK_SETTLE_READS,1); // the whole budget, floor to ceiling
 ramp(1000,116,1200,860,BC250_CLOCK_SETTLE_READS+1,1); // never arrives
 ramp(1200,110,1000,820,2,1);                        // a lowering that ramps is waited for the same way
 ramp(1200,110,1000,820,BC250_CLOCK_SETTLE_READS+7,0);
 for(path=0;path<2;path++)for(step=1;step<=(path?8u:6u);step++){
  struct backend b=setup(path?1000:1500,path?116:104,path?1500:1000,path?919:820);
  struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.fail_message=(int)step;
  CHECK(bc250_clock_prepare(&c,b.target_mhz,path?919:820,&r)==-74);CHECK(!r.ready);
  CHECK(r.messages_attempted==step && r.messages_completed==step-1);
  CHECK(b.count==step && b.end_count==1 && !b.held);
  if(path && step<=4)CHECK(b.mhz==1000); // no early frequency request
  if(!path && step==4)CHECK(b.mhz==1000 && b.vid==104); // keep completed downclock
 }
 {struct backend b=setup(1000,116,1200,860);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  // A transport failure on a re-read ends the transaction there.
  b.ramp_reads=3;b.fail_message=9;
  CHECK(bc250_clock_prepare(&c,1200,860,&r)==-74);CHECK(!r.ready && r.settle_reads==2 && b.delays==2);
  CHECK(r.messages_attempted==9 && r.messages_completed==8 && b.end_count==1 && !b.held);
 }
 for(step=4;step<=8;step+=2){
  struct backend b=setup(1000,116,1500,919);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  b.bad_read=step==6?7:step;
  CHECK(bc250_clock_prepare(&c,1500,919,&r)==BC250_CLOCK_MISMATCH);CHECK(!r.ready && b.end_count==1 && !b.held);
  CHECK(!r.settle_reads && !b.delays); // 1501 is past the request, not on the way: no re-read
  if(step==4)CHECK(b.mhz==1000 && b.count==4);
 }
 {struct backend b=setup(1000,116,1200,860);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  // Below where it started is not on the way up either.
  b.bad_read=7;b.read_delta=-201;
  CHECK(bc250_clock_prepare(&c,1200,860,&r)==BC250_CLOCK_MISMATCH);
  CHECK(!r.ready && r.observed_mhz==999 && !r.settle_reads && b.count==8);
 }
 {struct backend b=setup(1000,116,1000,820);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  // Nothing is on the way when the clock does not change.
  b.bad_read=5;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_MISMATCH);
  CHECK(!r.ready && r.observed_mhz==1001 && !r.settle_reads && b.count==6);
 }
 for(step=0;step<2;step++){
  struct backend b=setup(step?1000:0,step?256:116,1000,820);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_STATE_INVALID);CHECK(b.count==2 && !r.ready && b.end_count==1);
 }
 {struct backend b=setup(1000,116,1500,919);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=85000;
  // Hot: a raise is refused after the two readbacks and before any request.
  CHECK(bc250_clock_prepare(&c,1500,919,&r)==BC250_CLOCK_TOO_HOT);CHECK(!r.ready && b.count==2 && b.end_count==1);
  CHECK(b.mhz==1000 && b.vid==116 && r.initial_mhz==1000 && r.initial_vid==116);
  b.temp=67000;b.fail_temp=1;CHECK(bc250_clock_prepare(&c,1000,820,&r)==-72);CHECK(!r.ready && b.count==2 && b.end_count==2);
 }
 {struct backend b=setup(1000,136,1000,820);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=90000;
  // A voltage-only raise is a raise as well.
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_TOO_HOT);CHECK(!r.ready && b.count==2 && b.vid==136);
 }
 {struct backend b=setup(2000,88,1000,820);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=95000;
  // A hot part can always be clocked down: frequency first, then voltage.
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==0);CHECK(r.ready && b.mhz==1000 && b.vid==116 && r.temperature_mc==95000);
  CHECK(b.msg[2]==PPSMC_MSG_RequestGfxclk && b.msg[3]==PPSMC_MSG_ForceGfxVid);
 }
 {struct backend b=setup(2000,88,1500,919);struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=88000;
  CHECK(bc250_clock_prepare(&c,1500,919,&r)==0);CHECK(r.ready && b.mhz==1500 && b.vid==100);
 }
 positive(1000,116,2000,1000); // the ceiling from the floor: VID 88 staged before the clock
 positive(2000,88,1000,820);   // and back
 {struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.fail_begin=1;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==-71);CHECK(!r.ready && !b.end_count && !b.count && !b.temp_count);
 }
 {struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  CHECK(bc250_clock_prepare(&c,1501,820,&r)==BC250_CLOCK_INVALID);    // off the 100 MHz grid
  CHECK(bc250_clock_prepare(&c,2100,1000,&r)==BC250_CLOCK_INVALID);   // above the ceiling clock
  CHECK(bc250_clock_prepare(&c,900,820,&r)==BC250_CLOCK_INVALID);     // below the floor clock
  CHECK(bc250_clock_prepare(&c,1000,1001,&r)==BC250_CLOCK_INVALID);   // above the ceiling voltage
  CHECK(bc250_clock_prepare(&c,1000,819,&r)==BC250_CLOCK_INVALID);    // below the table
  CHECK(bc250_clock_prepare(&c,1500,900,&r)==BC250_CLOCK_INVALID);    // the old lab ceiling is below the table
  CHECK(bc250_clock_prepare(&c,2000,999,&r)==BC250_CLOCK_INVALID);
  CHECK(!b.begin_count && !b.count && !r.ready);
  c.end=NULL;CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_INVALID);CHECK(!b.begin_count);
 }
 printf("clock policy: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
