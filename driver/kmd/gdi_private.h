#ifndef BC250_GDI_PRIVATE_H
#define BC250_GDI_PRIVATE_H

// Windows ABI: 32-bit unsigned long, 64-bit unsigned long long.
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul    // "LB7A"
typedef struct _BC250_WDDM_ALLOCATION_PRIVATE {
    unsigned long Magic;
    unsigned long Version;
    unsigned long Width;
    unsigned long Height;
    unsigned long Pitch;
    unsigned long Format;                       // D3DDDIFORMAT
    unsigned long long Size;
} BC250_WDDM_ALLOCATION_PRIVATE;

// Optional standard-GDI trailer. Keep the 32-byte LB7A v1 prefix unchanged:
// deployed UMD OpenResource accepts >=32 bytes and consumes only that prefix.
#define BC250_GDI_PRIVATE_MAGIC 0x31494447ul // "GDI1"
typedef struct _BC250_GDI_PRIVATE {
    BC250_WDDM_ALLOCATION_PRIVATE Surface;
    unsigned long Magic, Type, Flags, Reserved;
} BC250_GDI_PRIVATE;

static __inline int WddmGdiPrivate(const void* Data, unsigned int Bytes, unsigned long* Type)
{
    const BC250_GDI_PRIVATE* gdi=(const BC250_GDI_PRIVATE*)Data;
    if (!Type) return 0;
    *Type=0;
    if (!Data) return 0;
    if (Bytes==sizeof(BC250_WDDM_ALLOCATION_PRIVATE)) return 1;
    if (Bytes!=sizeof(*gdi) || gdi->Magic!=BC250_GDI_PRIVATE_MAGIC ||
        gdi->Surface.Version!=1 || !gdi->Type || gdi->Type>8 ||
        gdi->Flags || gdi->Reserved) return 0;
    *Type=gdi->Type;
    return 1;
}

#endif
