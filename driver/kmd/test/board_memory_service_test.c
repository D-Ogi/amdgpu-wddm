#include <stdio.h>
#include <string.h>
#include "board_memory_service.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL CHECK %u: %s\n", __LINE__, #x); } } while (0)
struct fixture {
 unsigned char hw[28], disk[28];
 unsigned pending, exists, loads, saves, writes, reads, acknowledged, backup_durable;
 unsigned fail_load, bad_load, fail_save, fail_write;
 int all_writes_fail, all_reads_fail, fail_reads_after_write, disk_bad, order_bad;
};
static void block(unsigned char* b, unsigned mib) {
 unsigned i,sum=0; memset(b,0,28); b[0]=0x24;b[1]=0x41;b[2]=0x42;b[3]=0x4c;
 for(i=6;i<26;++i)b[i]=(unsigned char)i;
 b[26]=(unsigned char)mib;b[27]=(unsigned char)(mib>>8);
 for(i=6;i<28;++i)sum+=b[i];b[4]=(unsigned char)sum;b[5]=(unsigned char)(sum>>8);
}
static int rd(void* c,unsigned off,unsigned char* value) {
 struct fixture* f=c;++f->reads;if(f->all_reads_fail || (f->fail_reads_after_write && f->writes))return 0;*value=f->hw[off];return 1;
}
static int wr(void* c,unsigned off,unsigned char value) {
 struct fixture* f=c;++f->writes;
 if(!f->exists || f->pending!=1 || !f->acknowledged) f->order_bad=1;
 if(f->all_writes_fail || f->writes==f->fail_write)return 0;
 f->hw[off]=value;return 1;
}
static int load(void* c,unsigned char out[28],unsigned* pending) {
 struct fixture* f=c;++f->loads;
 if(f->disk_bad || f->loads==f->fail_load)return -1;
 if(!f->exists)return 0;
 memcpy(out,f->disk,28);*pending=f->pending;
 if(f->loads==f->bad_load)out[8]^=1;
 else if(f->pending==1)f->acknowledged=1; else f->backup_durable=1;
 return 1;
}
static int save(void* c,const unsigned char in[28],unsigned pending) {
 struct fixture* f=c;++f->saves;
 if(f->saves==f->fail_save)return 0; /* Includes flush/read-back failure at store boundary. */
 if(pending && !f->backup_durable)f->order_bad=1;
 if(f->exists && memcmp(f->disk,in,28))f->order_bad=1;
 memcpy(f->disk,in,28);f->exists=1;f->pending=pending;f->acknowledged=0;return 1;
}
static void setup(struct fixture* f,struct board_memory_state* s,struct bc250_uma_io* io,struct board_memory_store* st) {
 memset(f,0,sizeof(*f));block(f->hw,8192);
 io->context=f;io->read=rd;io->write=wr;st->context=f;st->load=load;st->save=save;
 BoardMemoryServiceStart(s,io,st);CHECK(s->ready && !s->blocked);
 f->loads=f->reads=0;
}
int main(void) {
 struct fixture f;struct board_memory_state s,restarted;struct bc250_uma_io io;struct board_memory_store st;
 unsigned char expected[28],original[28]; unsigned i;int rc;
 setup(&f,&s,&io,&st);memcpy(original,f.hw,28);memcpy(expected,f.hw,28);
 rc=BoardMemoryServiceChange(&s,&io,&st,expected,12288,0);
 CHECK(rc==BC250_UMA_OK && f.writes>0 && f.saves==3 && f.loads==4 && !f.order_bad);
 CHECK(f.pending==0 && s.backup_valid && !memcmp(f.disk,original,28));
 memcpy(expected,f.hw,28);rc=BoardMemoryServiceChange(&s,&io,&st,expected,8192,0);
 CHECK(rc==BC250_UMA_OK && !memcmp(f.disk,original,28) && !f.order_bad);
 memcpy(expected,f.hw,28);rc=BoardMemoryServiceChange(&s,&io,&st,expected,0,1);
 CHECK(rc==BC250_UMA_OK && !memcmp(f.hw,original,28) && !memcmp(f.disk,original,28));
 f.writes=0;rc=BoardMemoryServiceChange(&s,&io,&st,f.hw,0,1);
 CHECK(rc==BC250_UMA_NO_CHANGE && !f.writes);
 setup(&f,&s,&io,&st);rc=BoardMemoryServiceChange(&s,&io,&st,f.hw,8192,0);
 CHECK(rc==BC250_UMA_NO_CHANGE && !f.writes && !f.saves);
 memcpy(expected,f.hw,28);expected[6]^=1;
 CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_INVALID && !f.writes);
 block(expected,12288);CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_STALE && !f.writes);
 /* Save encompasses flush/read-back; each pre-write save failure must stop hardware. */
 for(i=1;i<=3;++i) {
  setup(&f,&s,&io,&st);f.fail_save=i;memcpy(expected,f.hw,28);
  CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_ROLLBACK_UNCONFIRMED && s.blocked);
  CHECK(i==3 ? (f.writes>0 && f.pending==1) : f.writes==0);
 }
 for(i=1;i<=4;++i) {
  setup(&f,&s,&io,&st);f.fail_load=i;memcpy(expected,f.hw,28);
  CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_ROLLBACK_UNCONFIRMED && s.blocked);
  CHECK(i==4 ? f.writes>0 : f.writes==0);
 }
 for(i=2;i<=3;++i) {
  setup(&f,&s,&io,&st);f.bad_load=i;memcpy(expected,f.hw,28);
  CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_ROLLBACK_UNCONFIRMED && s.blocked && !f.writes);
 }
 setup(&f,&s,&io,&st);f.exists=1;block(f.disk,8192);f.pending=1;
 BoardMemoryServiceStart(&restarted,&io,&st);CHECK(restarted.blocked && !restarted.ready);
 CHECK(BoardMemoryServiceChange(&restarted,&io,&st,f.hw,12288,0)==BC250_UMA_INVALID && !f.writes);
 setup(&f,&s,&io,&st);f.exists=1;block(f.disk,8192);f.disk[4]^=1;
 BoardMemoryServiceStart(&restarted,&io,&st);CHECK(restarted.blocked && !restarted.ready && !f.writes);
 setup(&f,&s,&io,&st);f.exists=1;block(f.disk,8192);f.disk[6]++;f.disk[4]++;
 BoardMemoryServiceStart(&restarted,&io,&st);CHECK(restarted.blocked && !restarted.ready); /* another timing/board snapshot */
 setup(&f,&s,&io,&st);f.exists=1;block(f.disk,8192);
 BoardMemoryServiceStart(&s,&io,&st);f.exists=0;
 CHECK(BoardMemoryServiceChange(&s,&io,&st,f.hw,12288,0)==BC250_UMA_ROLLBACK_UNCONFIRMED && !f.writes);
 setup(&f,&s,&io,&st);f.all_writes_fail=1;memcpy(expected,f.hw,28);
 CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_ROLLBACK_UNCONFIRMED && f.pending==1 && s.blocked);
 BoardMemoryServiceStart(&restarted,&io,&st);CHECK(restarted.blocked);
 setup(&f,&s,&io,&st);f.fail_write=2;memcpy(expected,f.hw,28);
 CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_RESTORED && !memcmp(f.hw,expected,28) && !f.pending && !s.blocked);
 setup(&f,&s,&io,&st);f.all_reads_fail=1;
 CHECK(BoardMemoryServiceChange(&s,&io,&st,f.hw,12288,0)==BC250_UMA_READ_FAILED && !f.writes && !f.saves);
 setup(&f,&s,&io,&st);f.fail_reads_after_write=1;memcpy(expected,f.hw,28);
 CHECK(BoardMemoryServiceChange(&s,&io,&st,expected,12288,0)==BC250_UMA_ROLLBACK_UNCONFIRMED && f.pending==1 && s.blocked);
 setup(&f,&s,&io,&st);f.exists=1;block(f.disk,8192);BoardMemoryServiceStart(&s,&io,&st);
 f.disk[0]=0x41;f.disk[1]=0x50;f.disk[2]=0x43;f.disk[3]=0x42;
 CHECK(BoardMemoryServiceChange(&s,&io,&st,f.hw,12288,0)==BC250_UMA_ROLLBACK_UNCONFIRMED && !f.writes && s.blocked);
 setup(&f,&s,&io,&st);f.disk_bad=1;BoardMemoryServiceStart(&restarted,&io,&st);
 CHECK(restarted.blocked && !restarted.ready && !f.writes);
 printf("Board memory service: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
