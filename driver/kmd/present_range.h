#pragma once

// Validate a CPU-mapped VRAM range against every GPU page. This witnesses
// translation only; the caller still owns synchronization and mapping lifetime.
typedef int (*BC250_RANGE_TRANSLATE)(void* Context, unsigned long long Va,
    unsigned long long* Physical, int* System);
static __inline int Bc250PresentVramRange(void* Context, BC250_RANGE_TRANSLATE Translate,
    unsigned long long Va, unsigned long long Bytes,
    unsigned long long* First, unsigned long long* Last)
{
    unsigned long long done=0,base=0;
    if (!Translate || !First || !Last || !Bytes || Va>~0ull-Bytes) return 0;
    while (done<Bytes) {
        unsigned long long physical=0,step=4096ull-((Va+done)&4095ull);
        int system=0;
        if (!Translate(Context,Va+done,&physical,&system) || system) return 0;
        if (!done) {
            base=physical;
            if (base>~0ull-Bytes) return 0;
        }
        if (physical!=base+done) return 0;
        if (step>Bytes-done) step=Bytes-done;
        done+=step;
    }
    *First=base; *Last=base+Bytes-1;
    return 1;
}
