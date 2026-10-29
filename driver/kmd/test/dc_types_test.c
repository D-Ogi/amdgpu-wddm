// Compare the actual kernel shim with the host stdbool.h contract.
#include <stdio.h>
#include "bc250_dc_types.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static bool Convert(unsigned value) { return (bool)value; }
typedef char BoolSize[(sizeof(bool)==1)?1:-1];
typedef char SignedSizes[(sizeof(int8_t)==1 && sizeof(int16_t)==2 && sizeof(int32_t)==4 && sizeof(int64_t)==8)?1:-1];
typedef char UnsignedSizes[(sizeof(uint8_t)==1 && sizeof(uint16_t)==2 && sizeof(uint32_t)==4 && sizeof(uint64_t)==8)?1:-1];
int main(void)
{
    static const unsigned values[] = {0,1,2,127,128,255,256,257,65536,0x80000000u,0xffffffffu};
    unsigned i;
    for(i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        volatile unsigned input=values[i];
        bool direct=(bool)input;
        struct { bool flag; } field={0};
        field.flag=(bool)input;
        CHECK(direct==(input!=0));
        CHECK(field.flag==(input!=0));
        CHECK(Convert(input)==(input!=0));
    }
    printf("DC types: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
