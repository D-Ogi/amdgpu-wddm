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
        for (j=0;j<64;j++) Slots[i].Known[j]=0;
        // Entries stay uninitialized until at least one byte is written.
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
        slot->Known[index/8]|=255ull<<((index%8)*8);
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
    if (!slot || !slot->Occupied || ((slot->Known[Index/8]>>((Index%8)*8))&255)!=255) return PAGING_PT_MISSING;
    *Entry=slot->Entries[Index];
    return PAGING_PT_OK;
}

unsigned PagingPtShadowTableCount(PAGING_PT_U64 VirtualBytes, unsigned Levels)
{
    PAGING_PT_U64 pages,total=0;
    unsigned i;
    if (!VirtualBytes || VirtualBytes>(1ull<<48) || !Levels || Levels>4) return 0;
    pages=(VirtualBytes+4095)/4096;
    for(i=0;i<Levels;i++) { pages=(pages+511)/512;total+=pages; }
    if(pages!=1 || total>0xffffffffu)return 0;
    return (unsigned)total;
}

int PagingPtShadowCanApply(const PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                          unsigned First, unsigned Count, int Register)
{
    PAGING_PT_SHADOW_SLOT* slot;
    if(!State || !State->Slots || !State->Capacity || !ValidTable(Physical) ||
       First>=PAGING_PT_ENTRIES || !Count || Count>PAGING_PT_ENTRIES-First)return PAGING_PT_INVALID;
    slot=Find(State,Physical);
    if(!slot)return Register?PAGING_PT_FULL:PAGING_PT_MISSING;
    if(!slot->Occupied && !Register)return PAGING_PT_MISSING;
    return PAGING_PT_OK;
}

int PagingPtShadowCopy(PAGING_PT_SHADOW* State, PAGING_PT_U64 Source, unsigned SourceFirst,
                      PAGING_PT_U64 Destination, unsigned DestinationFirst, unsigned Count)
{
    PAGING_PT_SHADOW_SLOT *source,*destination;
    unsigned i;
    int reverse;
    if(!State || !State->Slots || !State->Capacity || !ValidTable(Source) || !ValidTable(Destination) ||
       SourceFirst>=PAGING_PT_ENTRIES || DestinationFirst>=PAGING_PT_ENTRIES || !Count ||
       Count>PAGING_PT_ENTRIES-SourceFirst || Count>PAGING_PT_ENTRIES-DestinationFirst)
        return PAGING_PT_INVALID;
    destination=Find(State,Destination);
    if(!destination || !destination->Occupied)return PAGING_PT_MISSING;
    source=Find(State,Source);
    if(source && !source->Occupied)source=0;
    reverse=source==destination && DestinationFirst>SourceFirst && DestinationFirst-SourceFirst<Count;
    for(i=0;i<Count;i++) {
        unsigned at=reverse?Count-1-i:i;
        unsigned src=SourceFirst+at,dst=DestinationFirst+at;
        unsigned shift=(dst%8)*8;
        PAGING_PT_U64 mask=255ull<<shift;
        PAGING_PT_U64 known=source ? (source->Known[src/8]>>((src%8)*8))&255 : 0;
        // A first partial fill initialized the whole storage word, but only
        // the marked halves represent known data. No read when both are unknown.
        if(known) destination->Entries[dst]=source->Entries[src];
        destination->Known[dst/8]=(destination->Known[dst/8]&~mask)|(known<<shift);
    }
    return PAGING_PT_OK;
}

int PagingPtShadowFill(PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                       PAGING_PT_U64 Bytes, unsigned Pattern)
{
    unsigned slotIndex;
    PAGING_PT_U64 end,pattern=((PAGING_PT_U64)Pattern<<32)|Pattern;
    if (!State || !State->Slots || !State->Capacity || !Bytes ||
        ((Physical|Bytes)&3)!=0 || Physical>0xffffffffffffull ||
        Bytes>0x1000000000000ull-Physical) return PAGING_PT_INVALID;
    end=Physical+Bytes;
    // Scan only registered-table storage, not every page in a large fill range.
    for(slotIndex=0;slotIndex<State->Capacity;slotIndex++) {
        PAGING_PT_SHADOW_SLOT* slot=&State->Slots[slotIndex];
        PAGING_PT_U64 begin,limit;
        unsigned first,last,index;
        if (!slot->Occupied || slot->Physical>=end || slot->Physical+4096<=Physical) continue;
        begin=Physical>slot->Physical ? Physical-slot->Physical : 0;
        limit=end-slot->Physical;if(limit>4096)limit=4096;
        first=(unsigned)(begin/8);last=(unsigned)((limit-1)/8);
        for(index=first;index<=last;index++) {
            unsigned shift=(index%8)*8;
            PAGING_PT_U64 known=(slot->Known[index/8]>>shift)&255;
            unsigned low=begin<=(PAGING_PT_U64)index*8;
            unsigned high=limit>=(PAGING_PT_U64)index*8+8;
            if(low && high) {
                slot->Entries[index]=pattern;known=255;
            } else {
                // Zero initializes storage only. The unwritten half stays unknown.
                PAGING_PT_U64 old=known ? slot->Entries[index] : 0;
                slot->Entries[index]=low ? (old&0xffffffff00000000ull)|Pattern :
                    (old&0xffffffffull)|((PAGING_PT_U64)Pattern<<32);
                known|=low?15u:240u;
            }
            slot->Known[index/8]|=known<<shift;
        }
    }
    return PAGING_PT_OK;
}

// Caller serializes source, destination and external scratch through publication.
static int CopySlotBytes(const PAGING_PT_SHADOW_SLOT* source,unsigned srcOffset,
    PAGING_PT_SHADOW_SLOT* destination,unsigned dstOffset,unsigned Bytes)
{
    unsigned i;
    int reverse;
    reverse=source==destination && dstOffset>srcOffset && dstOffset-srcOffset<Bytes;
    for(i=0;i<Bytes;i++) {
        unsigned at=reverse?Bytes-1-i:i,src=srcOffset+at,dst=dstOffset+at;
        PAGING_PT_U64 mask=1ull<<(dst%64);
        if(source && (source->Known[src/64]&(1ull<<(src%64)))) {
            unsigned shift=(dst%8)*8;
            PAGING_PT_U64 value=(source->Entries[src/8]>>((src%8)*8))&255;
            PAGING_PT_U64 old=((destination->Known[dst/64]>>((dst/8%8)*8))&255) ? destination->Entries[dst/8] : 0;
            destination->Entries[dst/8]=(old&~(255ull<<shift))|(value<<shift);
            destination->Known[dst/64]|=mask;
        } else destination->Known[dst/64]&=~mask;
    }
    return PAGING_PT_OK;
}

int PagingPtShadowCopyBytes(PAGING_PT_SHADOW* State, PAGING_PT_U64 Source,
    PAGING_PT_U64 Destination, unsigned Bytes, int SourceTracked)
{
    PAGING_PT_SHADOW_SLOT *source,*destination;
    unsigned srcOffset=(unsigned)(Source&4095),dstOffset=(unsigned)(Destination&4095);
    if (!State || !State->Slots || !State->Capacity || !Bytes ||
        (SourceTracked!=0 && SourceTracked!=1) || Source>0xffffffffffffull || Destination>0xffffffffffffull ||
        Bytes>4096u-srcOffset || Bytes>4096u-dstOffset) return PAGING_PT_INVALID;
    destination=Find(State,Destination&~4095ull);
    if (!destination || !destination->Occupied) return PAGING_PT_MISSING;
    source=SourceTracked ? Find(State,Source&~4095ull) : 0;
    if (source && !source->Occupied) source=0;
    return CopySlotBytes(source,srcOffset,destination,dstOffset,Bytes);
}

int PagingPtShadowSaveBytes(PAGING_PT_SHADOW* State,PAGING_PT_U64 Source,
    unsigned Bytes,int SourceTracked,PAGING_PT_SHADOW_SLOT* Scratch)
{
    PAGING_PT_SHADOW_SLOT* source;
    unsigned offset=(unsigned)(Source&4095),i;
    if(!State || !State->Slots || !State->Capacity || !Scratch || !Bytes ||
       Bytes>4096-offset || Source>0xffffffffffffull ||
       (SourceTracked!=0 && SourceTracked!=1))return PAGING_PT_INVALID;
    source=SourceTracked?Find(State,Source&~4095ull):0;
    if(source && !source->Occupied)source=0;
    // Scratch is caller-owned and separate from registered table slots. A new
    // SAVE invalidates the complete old scratch, including bytes outside this band.
    Scratch->Occupied=0;Scratch->Physical=0;
    for(i=0;i<64;i++)Scratch->Known[i]=0;
    return CopySlotBytes(source,offset,Scratch,0,Bytes);
}

int PagingPtShadowRestoreBytes(PAGING_PT_SHADOW* State,const PAGING_PT_SHADOW_SLOT* Scratch,
    PAGING_PT_U64 Destination,unsigned Bytes)
{
    PAGING_PT_SHADOW_SLOT* destination;
    unsigned offset=(unsigned)(Destination&4095);
    if(!State || !State->Slots || !State->Capacity || !Scratch || !Bytes ||
       Bytes>4096-offset || Destination>0xffffffffffffull)return PAGING_PT_INVALID;
    destination=Find(State,Destination&~4095ull);
    if(!destination || !destination->Occupied)return PAGING_PT_MISSING;
    return CopySlotBytes(Scratch,0,destination,offset,Bytes);
}
