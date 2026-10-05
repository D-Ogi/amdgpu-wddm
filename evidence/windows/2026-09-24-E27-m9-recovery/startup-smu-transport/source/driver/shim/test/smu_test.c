/* Whole clock transaction through the real imported mailbox bodies. */
#include <stdio.h>
#include <string.h>
#include "bc250_clock.h"
#include "bc250_smu.h"
#include "smu_v11_8_ppsmc.h"
#include "generated/smu_registers.h"
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n",__LINE__,#x); } } while (0)
#define MSG BC250_SMU_mmMP1_SMN_C2PMSG_66
#define ARG BC250_SMU_mmMP1_SMN_C2PMSG_82
#define RESP BC250_SMU_mmMP1_SMN_C2PMSG_90
struct model {
    struct bc250_smu smu;
    struct bc250_smu_report last;
    unsigned held, begin_count, end_count, temp_count;
    unsigned phase, response, argument, command, pending, countdown;
    unsigned mhz, vid, messages, reads, writes, arg_reads, delays;
    unsigned reject, never_complete, frozen_time, delay_scale, fail_read;
    unsigned long long now;
};
static int owned(void *c) { return ((struct model *)c)->held == 1; }
static int begin(void *c) {
    struct model *m=c; CHECK(!m->held); m->held=1; m->begin_count++; return 0;
}
static void end(void *c) {
    struct model *m=c; CHECK(m->held==1); m->held=0; m->end_count++;
}
static int temperature(void *c,int *value) {
    struct model *m=c; CHECK(m->held==1); CHECK(!m->pending);
    m->temp_count++; *value=67000; return 0;
}
static int read_reg(void *c,unsigned reg,unsigned *value) {
    struct model *m=c; CHECK(m->held==1); m->reads++;
    if(m->fail_read) return -700;
    if(reg==RESP) {
        if(m->pending && !m->never_complete && !--m->countdown) {
            m->pending=0; m->response=m->reject?m->reject:1;
            if(m->response==1) {
                switch(m->command) {
                case PPSMC_MSG_RequestGfxclk: m->mhz=m->argument; break;
                case PPSMC_MSG_ForceGfxVid: m->vid=m->argument; break;
                case PPSMC_MSG_GetGfxFrequency: m->argument=m->mhz; break;
                case PPSMC_MSG_GetGfxVid: m->argument=m->vid; break;
                default: CHECK(0); break;
                }
            }
        }
        *value=m->response;
    } else if(reg==ARG) {
        CHECK(!m->pending && m->response==1); m->arg_reads++; *value=m->argument;
    } else { CHECK(0); return -701; }
    return 0;
}
static int write_reg(void *c,unsigned reg,unsigned value) {
    static const unsigned sequence[4]={PPSMC_MSG_RequestGfxclk,PPSMC_MSG_ForceGfxVid,
        PPSMC_MSG_GetGfxFrequency,PPSMC_MSG_GetGfxVid};
    static const unsigned parameters[4]={1000,116,0,0};
    struct model *m=c; CHECK(m->held==1); CHECK(!m->pending);
    CHECK(m->temp_count==m->begin_count && m->temp_count>0); m->writes++;
    if(reg==RESP) {
        CHECK(m->phase==0 && value==0); m->response=value; m->phase=1;
    } else if(reg==ARG) {
        CHECK(m->phase==1 && m->response==0); m->argument=value; m->phase=2;
    } else if(reg==MSG) {
        CHECK(m->phase==2 && m->response==0);
        CHECK(value==sequence[m->messages%4]);
        CHECK(m->argument==parameters[m->messages%4]);
        m->command=value; m->messages++; m->pending=1; m->countdown=3; m->phase=0;
    } else { CHECK(0); return -702; }
    return 0;
}
static unsigned long long now_us(void *c) { return ((struct model *)c)->now; }
static void delay_us(void *c,unsigned us) {
    struct model *m=c; CHECK(m->held==1 && us==1); m->delays++;
    if(!m->frozen_time) m->now+=(unsigned long long)us*m->delay_scale;
}
static int message(void *c,unsigned msg,unsigned param,unsigned *value) {
    struct model *m=c; int status=bc250_smu_message_locked(&m->smu,msg,param,&m->last);
    if(!status) *value=m->last.value;
    return status;
}
static void setup(struct model *m,int active) {
    struct bc250_smu_io io={m,owned,read_reg,write_reg,now_us,delay_us};
    memset(m,0,sizeof(*m)); m->response=active?1:0; m->delay_scale=1;
    CHECK(bc250_smu_init(&m->smu,&io,10,active)==0);
}
static int prepare(struct model *m,struct bc250_clock_report *report) {
    struct bc250_clock_io io={m,begin,end,temperature,message};
    return bc250_clock_prepare(&io,1000,820,report);
}
static void positive(int active) {
    struct model m; struct bc250_clock_report report; unsigned iteration;
    setup(&m,active);
    for(iteration=1;iteration<=2;iteration++) {
        CHECK(prepare(&m,&report)==0); CHECK(report.ready==1);
        CHECK(report.observed_mhz==1000 && report.observed_vid==116);
        CHECK(m.mhz==1000 && m.vid==116);
        CHECK(m.messages==4*iteration && m.writes==12*iteration);
        CHECK(m.arg_reads==4*iteration && m.delays==8*iteration);
        CHECK(m.reads==(active?20u:19u)+20*(iteration-1));
        CHECK(m.begin_count==iteration && m.end_count==iteration && !m.held);
        CHECK(report.messages_attempted==4 && report.messages_completed==4);
        CHECK(m.last.prior_response==1 && m.last.response==1 && m.last.value==116);
        CHECK(m.last.writes==3 && m.last.polls==4 && !m.last.io_status);
    }
}
int main(void) {
    unsigned mode;
    positive(0); positive(1);
    { struct model m; struct bc250_smu_report report; setup(&m,1);
      CHECK(bc250_smu_message_locked(&m.smu,PPSMC_MSG_GetGfxVid,0,&report)==BC250_SMU_NOT_OWNER);
      CHECK(!m.reads && !m.writes && !report.message_written);
      m.held=2; /* A different caller owns it: globally held is not permission. */
      CHECK(bc250_smu_message_locked(&m.smu,PPSMC_MSG_GetGfxVid,0,&report)==BC250_SMU_NOT_OWNER);
      CHECK(!m.reads && !m.writes);
    }
    { struct model m; struct bc250_clock_report report; setup(&m,1); m.reject=0xff;
      CHECK(prepare(&m,&report)==-5); CHECK(!report.ready && !report.messages_completed);
      CHECK(m.messages==1 && m.writes==3 && !m.arg_reads);
      CHECK(m.last.response==0xff && m.last.status==-5 && !m.held);
    }
    for(mode=0;mode<3;mode++) {
      struct model m; struct bc250_clock_report report; setup(&m,1);
      m.never_complete=1; m.frozen_time=mode==1; m.delay_scale=mode==2?5:1;
      CHECK(prepare(&m,&report)==-62); CHECK(!report.ready && !report.messages_completed);
      CHECK(m.messages==1 && m.writes==3 && !m.arg_reads && !m.held);
      CHECK(m.last.response==0 && m.last.prior_response==1);
      CHECK(m.last.polls==(mode==2?4u:11u));
      CHECK(m.last.delays==(mode==2?2u:10u));
    }
    { struct model m; struct bc250_clock_report report; setup(&m,1); m.response=0;
      CHECK(prepare(&m,&report)==-62); CHECK(!report.ready);
      CHECK(!m.messages && !m.writes && m.last.polls==10 && !m.held);
    }
    { struct model m; struct bc250_clock_report report; setup(&m,1); m.fail_read=1;
      CHECK(prepare(&m,&report)==-700); CHECK(!report.ready && !m.writes);
      CHECK(m.last.io_status==-700 && !m.held);
    }
    printf("SMU integrated clock: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
