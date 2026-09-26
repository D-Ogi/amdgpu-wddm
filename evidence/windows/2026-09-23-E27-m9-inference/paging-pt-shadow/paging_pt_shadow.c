#include "paging_pt_shadow.h"

static int ValidTable(PAGING_PT_U64 Physical)
{
    return (Physical & 4095u)==0 && Physical<=0xfffffffff000ull;
}

// No deletions: paging-process tables stay pinned until the instance is reset.
static PAGING_PT_SHADOW_SLOT* Find(const PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical)
{
    unsigned start, i;
    if (!State || !State->Slots || !State->Capacity) return 0;
    start=(unsigned)((Physical>>12)%State->Capacity);
    for (i=0;i<State->Capacity;i++) {
        unsigned index=(unsigned)(((PAGING_PT_U64)start+i)%State->Capacity);
        PAGING_PT_SHADOW_SLOT* slot=&State->Slots[index];
        if (!slot->Occupied || slot->Physical==Physical) return slot;
    }
    return 0;
}

int PagingPtShadowInit(PAGING_PT_SHADOW* State, PAGING_PT_SHADOW_SLOT* Slots, unsigned Capacity)
{
    unsigned i,j;
    if (!State || !Slots || !Capacity) return PAGING_PT_INVALID;
    State->Slots=Slots;State->Capacity=Capacity;State->Used=0;
    for (i=0;i<Capacity;i++) {
        Slots[i].Occupied=0;Slots[i].Physical=0;
        for (j=0;j<8;j++) Slots[i].Known[j]=0;
        // Entries remain uninitialized and unreadable until their Known bit is set.
    }
    return PAGING_PT_OK;
}

int PagingPtShadowApply(PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                       unsigned First, unsigned Count, const PAGING_PT_U64* Entries, int Register)
{
    PAGING_PT_SHADOW_SLOT* slot;
    unsigned i;
    if (!State || !State->Slots || !State->Capacity || !ValidTable(Physical) || !Entries ||
        First>=PAGING_PT_ENTRIES || !Count || Count>PAGING_PT_ENTRIES-First)
        return PAGING_PT_INVALID;
    slot=Find(State,Physical);
    if (!slot) return Register ? PAGING_PT_FULL : PAGING_PT_MISSING;
    if (!slot->Occupied) {
        if (!Register) return PAGING_PT_MISSING;
        slot->Physical=Physical;slot->Occupied=1;State->Used++;
    }
    for (i=0;i<Count;i++) {
        unsigned index=First+i;
        slot->Entries[index]=Entries[i];
        slot->Known[index/64]|=1ull<<(index%64);
    }
    return PAGING_PT_OK;
}

int PagingPtShadowRead(const PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                      unsigned Index, PAGING_PT_U64* Entry)
{
    PAGING_PT_SHADOW_SLOT* slot;
    if (!Entry) return PAGING_PT_INVALID;
    *Entry=0;
    if (!State || !State->Slots || !State->Capacity || !ValidTable(Physical) || Index>=PAGING_PT_ENTRIES)
        return PAGING_PT_INVALID;
    slot=Find(State,Physical);
    if (!slot || !slot->Occupied || !(slot->Known[Index/64] & (1ull<<(Index%64)))) return PAGING_PT_MISSING;
    *Entry=slot->Entries[Index];
    return PAGING_PT_OK;
}
