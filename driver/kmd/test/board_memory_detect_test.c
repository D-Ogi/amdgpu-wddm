#include <stdio.h>
#include <string.h>
#include "uma_transport.h"
typedef unsigned long ULONG;
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define PASSIVE_LEVEL 0
#define KernelMode 0
#define FALSE 0
#define Cmos 0
#define NT_SUCCESS(x) ((x) >= 0)
static unsigned irql, reads, writes, waits, errors, checks, fail, allowed, scenario, epoch;
static unsigned char memory[28];
#define CHECK(x) do { ++checks; if (!(x)) { ++fail; printf("FAIL CHECK line %u\n", (unsigned)__LINE__); } } while(0)
static unsigned KeGetCurrentIrql(void) { return irql; }
static long KeDelayExecutionThread(int mode, int alertable, LARGE_INTEGER* delay) {
    CHECK(mode == KernelMode && !alertable && delay->QuadPart == -1250000);
    ++waits; CHECK(waits <= 24); ++epoch; return 0;
}
static ULONG HalSetBusDataByOffset(int type, ULONG bus, ULONG slot, void* out, ULONG offset, ULONG count) {
    (void)type;(void)bus;(void)slot;(void)out;(void)offset;(void)count; ++writes; return 0;
}
static ULONG HalGetBusDataByOffset(int type, ULONG bus, ULONG slot, void* out, ULONG offset, ULONG count) {
    unsigned mapping = slot ? 1 : 2, index = (unsigned)(slot + offset); unsigned char value = 0;
    (void)type; ++reads;
    if (bus == 1) {
        CHECK(index >= 0x90 && index + count <= 0xac && (count == 1 || count == 28));
        if (!(allowed & mapping)) return 0;
        if (scenario == 1) return count - 1;
        memcpy(out, memory + index - 0x90, count);
        if (scenario == 2 && reads % 2 == 0) ((unsigned char*)out)[0] ^= 1;
        if (scenario == 12 && epoch) ((unsigned char*)out)[0] ^= 1;
        return count;
    }
    CHECK(bus == 0 && count == 1 && (index == 0 || index == 10 || index == 11 || index == 13));
    if (scenario == 3) return 0;
    if (index == 10) value = scenario == 4 ? 0x80 : 0;
    if (index == 11) value = scenario == 5 ? 0x80 : (scenario == 6 ? 4 : 0);
    if (index == 13) value = scenario == 7 ? 0 : 0x80;
    if (index == 0) {
        unsigned second = (scenario == 8 ? 0 : epoch / 8) % 60;
        if (scenario == 9) value = 0x6a;
        else if (scenario == 10) value = (unsigned char)(second ? 0 : 0x59);
        else if (scenario == 11) value = (unsigned char)(second ? 2 : 0);
        else value = (unsigned char)(scenario == 6 ? second : ((second / 10) * 16 + second % 10));
    }
    *(unsigned char*)out = value; return 1;
}
/* PRODUCTION */
static void reset(unsigned mask, unsigned which) {
    allowed=mask; scenario=which; epoch=0; reads=0; writes=0; waits=0; irql=0;
    memset(memory,0,sizeof(memory)); memcpy(memory,"$ABL",4); memory[27]=32; memory[4]=32;
}
static void run(unsigned mask, unsigned which, int expected, unsigned selected) {
    struct bc250_uma_transport transport = {99}; unsigned char result[28]; unsigned i;
    reset(mask,which); memset(result,99,sizeof(result));
    CHECK(BoardMemoryDetect(&transport,result)==expected); CHECK(transport.mapping==selected); CHECK(writes==0);
    if(expected) CHECK(!memcmp(result,memory,28));
    else for(i=0;i<28;++i) CHECK(result[i]==0);
}
int main(void) {
    unsigned i; struct bc250_uma_transport transport={1}; unsigned char result[28];
    (void)errors;
    run(1,0,1,1); run(2,0,1,2); run(3,0,0,0); run(0,0,0,0);
    run(1,6,1,1); run(1,10,1,1);
    for(i=1;i<=12;++i) if(i!=6 && i!=10) run(1,i,0,0);
    reset(1,0); memory[4]^=1; CHECK(!BoardMemoryDetect(&transport,result)); CHECK(!transport.mapping);
    reset(1,0); memory[27]=16; memory[4]=16; CHECK(!BoardMemoryDetect(&transport,result));
    reset(1,0); irql=1; CHECK(!BoardMemoryDetect(&transport,result)); CHECK(reads==0);
    reset(1,0); CHECK(!BoardMemoryDetect(NULL,result)); CHECK(!BoardMemoryDetect(&transport,NULL)); CHECK(reads==0);
    printf("%u checks, %u failures\n",checks,fail); return fail ? 1:0;
}
