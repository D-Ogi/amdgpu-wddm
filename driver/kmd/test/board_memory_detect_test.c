#include <stdio.h>
#include <string.h>
#include "uma_transport.h"
typedef unsigned long ULONG;
typedef unsigned __int64 ULONG_PTR;
typedef unsigned char* PUCHAR;
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define PASSIVE_LEVEL 0
#define KernelMode 0
#define FALSE 0
#define Cmos 0
#define NT_SUCCESS(x) ((x) >= 0)
static unsigned irql, reads, writes, waits, checks, fail, allowed, scenario, epoch, selected, index_value;
static unsigned char memory[28];
#define CHECK(x) do { ++checks; if (!(x)) { ++fail; printf("FAIL CHECK line %u\n", (unsigned)__LINE__); } } while(0)
static unsigned KeGetCurrentIrql(void) { return irql; }
static long KeDelayExecutionThread(int mode, int alertable, LARGE_INTEGER* delay) {
    CHECK(mode == KernelMode && !alertable && delay->QuadPart == -1250000);
    ++waits; CHECK(waits <= 12); ++epoch; return 0;
}
static void WRITE_PORT_UCHAR(PUCHAR port, unsigned char value) {
    CHECK((ULONG_PTR)port == 0x72 && !selected && value >= 0x90 && value <= 0xab);
    if ((ULONG_PTR)port != 0x72) { ++writes; return; }
    selected=1;index_value=value;
}
static unsigned char READ_PORT_UCHAR(PUCHAR port) {
    unsigned char value;
    CHECK((ULONG_PTR)port == 0x73 && selected && index_value >= 0x90 && index_value <= 0xab);
    selected=0;++reads;
    if(!allowed || scenario == 1 || index_value < 0x90 || index_value > 0xab) return 0xff;
    value=memory[index_value-0x90];
    if ((scenario == 2 && reads % 29 == 0) || (scenario == 12 && epoch)) value^=1;
    return value;
}
static ULONG HalGetBusDataByOffset(int type, ULONG bus, ULONG slot, void* out, ULONG offset, ULONG count) {
    unsigned index = (unsigned)slot; unsigned char value = 0;
    CHECK(type == Cmos); ++reads;
    if (bus == 1) {
        /* The exact x64 HAL ignores bus and offset: no extended-bank ABL.
           Model its unrelated RTC bank, not the old adapter's assumption. */
        memset(out,0xff,count);
        if(index == 0 && count) *(unsigned char*)out=0x37;
        return count;
    }
    CHECK(offset == 0);
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
    allowed=mask; scenario=which; epoch=0; reads=0; writes=0; waits=0; irql=0;selected=0;
    memset(memory,0,sizeof(memory)); memcpy(memory,"$ABL",4); memory[27]=32; memory[4]=32;
}
static void run(unsigned mask, unsigned which, int expected, unsigned expected_mapping) {
    struct bc250_uma_transport transport = {99}; unsigned char result[28]; unsigned i;
    reset(mask,which); memset(result,99,sizeof(result));
    CHECK(BoardMemoryDetect(&transport,result)==expected); CHECK(transport.mapping==expected_mapping); CHECK(writes==0);
    if(expected) CHECK(!memcmp(result,memory,28));
    else for(i=0;i<28;++i) CHECK(result[i]==0);
}
int main(void) {
    unsigned i; struct bc250_uma_transport transport={1}; unsigned char result[28];
    run(1,0,1,3); run(0,0,0,0);
    run(1,6,1,3); run(1,10,1,3);
    for(i=1;i<=12;++i) if(i!=6 && i!=10) run(1,i,0,0);
    reset(1,0); memory[4]^=1; CHECK(!BoardMemoryDetect(&transport,result)); CHECK(!transport.mapping);
    reset(1,0); memory[0]='X'; CHECK(!BoardMemoryDetect(&transport,result)); CHECK(!transport.mapping);
    reset(1,0); memory[27]=16; memory[4]=16; CHECK(!BoardMemoryDetect(&transport,result));
    reset(1,0); irql=1; CHECK(!BoardMemoryDetect(&transport,result)); CHECK(reads==0);
    reset(1,0); CHECK(!BoardMemoryDetect(NULL,result)); CHECK(!BoardMemoryDetect(&transport,NULL)); CHECK(reads==0);
    reset(1,0);
    CHECK(HalGetBusDataByOffset(Cmos,1,0x90,result,0,28)==28 && !bc250_uma_validate(result));
    CHECK(HalGetBusDataByOffset(Cmos,1,0,result,0x90,28)==28 && !bc250_uma_validate(result));
    CHECK(BoardMemoryDetect(&transport,result) && !memcmp(result,memory,28));
    printf("%u checks, %u failures\n",checks,fail); return fail ? 1:0;
}
