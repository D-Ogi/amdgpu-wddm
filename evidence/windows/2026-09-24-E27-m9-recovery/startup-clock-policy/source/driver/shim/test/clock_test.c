#include <stdio.h>
#include <string.h>
#include "bc250_clock.h"
#include "smu_v11_8_ppsmc.h"
static int checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line%d: %s\n",__LINE__,#x);}}while(0)
struct backend {int held,begin_count,end_count,temp_count,fail_begin,fail_temp,fail_message,temp;
 unsigned int count,mhz,vid,bad_mhz,bad_vid,msg[4],param[4];};
static int begin(void* context){struct backend*b=context;b->begin_count++;if(b->fail_begin)return -71;CHECK(!b->held);b->held=1;return 0;}
static void end(void* context){struct backend*b=context;CHECK(b->held==1);b->held=0;b->end_count++;}
static int temperature(void*context,int*out){struct backend*b=context;CHECK(b->held==1);b->temp_count++;*out=b->temp;return b->fail_temp?-72:0;}
static int message(void*context,unsigned int msg,unsigned int param,unsigned int*out){
 struct backend*b=context;CHECK(b->held==1);CHECK(b->temp_count==1);CHECK(b->count<4);
 if(b->count>=4)return -73;
 b->msg[b->count]=msg;b->param[b->count]=param;b->count++;
 if(b->fail_message==(int)b->count)return -74;
 if(msg==PPSMC_MSG_RequestGfxclk)b->mhz=param;
 else if(msg==PPSMC_MSG_ForceGfxVid)b->vid=param;
 else if(msg==PPSMC_MSG_GetGfxFrequency)*out=b->mhz+b->bad_mhz;
 else if(msg==PPSMC_MSG_GetGfxVid)*out=b->vid+b->bad_vid;
 else{CHECK(0);return -75;}
 return 0;
}
static struct bc250_clock_io io(struct backend*b){struct bc250_clock_io r={b,begin,end,temperature,message};return r;}
static void positive(unsigned int mhz,unsigned int mv){
 struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
 b.temp=67000;memset(&r,0xA5,sizeof(r));
 CHECK(bc250_clock_prepare(&c,mhz,mv,&r)==0);CHECK(r.ready==1);CHECK(r.status==0);
 CHECK(b.begin_count==1&&b.end_count==1&&!b.held);CHECK(b.temp_count==1);
 CHECK(r.messages_attempted==4&&r.messages_completed==4);
 CHECK(b.msg[0]==PPSMC_MSG_RequestGfxclk&&b.param[0]==mhz);
 CHECK(b.msg[1]==PPSMC_MSG_ForceGfxVid&&b.param[1]==(1550-mv)*160/1000);
 CHECK(b.msg[2]==PPSMC_MSG_GetGfxFrequency&&b.param[2]==0);
 CHECK(b.msg[3]==PPSMC_MSG_GetGfxVid&&b.param[3]==0);
 CHECK(r.observed_mhz==mhz&&r.observed_vid==b.param[1]);
 if(mhz==1000&&mv==820)CHECK(r.expected_vid==116&&r.observed_vid==116); // unit A M22/M243 reference
 CHECK(r.requested_mhz==mhz&&r.requested_mv==mv&&r.temperature_mc==67000);
}
int main(void){
 int step;positive(1000,820);positive(1500,900);positive(1000,820);
 for(step=1;step<=4;step++){
  struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=67000;b.fail_message=step;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==-74);CHECK(!r.ready);
  CHECK(r.messages_attempted==(unsigned int)step&&r.messages_completed==(unsigned int)(step-1));
  CHECK(b.count==(unsigned int)step&&b.end_count==1&&!b.held);
 }
 for(step=0;step<2;step++){
  struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=67000;
  b.bad_mhz=step?0:1;b.bad_vid=step?1:0;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_MISMATCH);CHECK(!r.ready&&b.end_count==1&&!b.held);
 }
 {struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.temp=85000;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_TOO_HOT);CHECK(!r.ready&&!b.count&&b.end_count==1);
  b.temp=67000;b.fail_temp=1;CHECK(bc250_clock_prepare(&c,1000,820,&r)==-72);CHECK(!r.ready&&!b.count&&b.end_count==2);
 }
 {struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);b.fail_begin=1;
  CHECK(bc250_clock_prepare(&c,1000,820,&r)==-71);CHECK(!r.ready&&!b.end_count&&!b.count&&!b.temp_count);
 }
 {struct backend b={0};struct bc250_clock_report r;struct bc250_clock_io c=io(&b);
  CHECK(bc250_clock_prepare(&c,1501,820,&r)==BC250_CLOCK_INVALID);
  CHECK(bc250_clock_prepare(&c,1000,901,&r)==BC250_CLOCK_INVALID);CHECK(!b.begin_count&&!b.count&&!r.ready);
  c.end=NULL;CHECK(bc250_clock_prepare(&c,1000,820,&r)==BC250_CLOCK_INVALID);CHECK(!b.begin_count);
 }
 printf("clock policy: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
