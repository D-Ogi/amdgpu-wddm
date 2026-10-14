/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_RENDER_H
#define BC250_GDI_RENDER_H
#include "gdi_pixel.h"
/*
 * The contract layer of DxgkDdiRenderGdi: what the OS may ask for, what this driver admits, and
 * where in memory each admitted sub-rectangle lands. Plain C over plain integers, like
 * blit_plan.h, so that it is host-tested on this PC and not on the lab. wddm.c owns the one thin
 * step this file deliberately does not: copying the WDK's DXGK_RENDERKM_COMMAND fields into
 * BC250_GDI_OP, with C_ASSERTs tying every constant below to the WDK's own ABI.
 *
 * RenderGdi, not RenderKm: ref/ddi-display/d3dkmddi.md DXGKDDI_RENDERGDI says "DxgkDdiRenderGdi is
 * used when submitting Windows Graphics Device Interface (GDI) commands for contexts that support
 * virtual addressing", and every context this adapter creates declares VirtualAddressing (GpuMmu).
 * DXGKARG_RENDERGDI carries DmaBufferGpuVirtualAddress and has no patch-location list at all,
 * which is why nothing here patches and the context keeps Caps.NoPatchingRequired.
 *
 * Geometry only, and offsets only. Nothing here is a GPU virtual address and nothing here proves
 * residency, allocation lifetime or cache state; the caller adds the allocation-list base and
 * carries those obligations, exactly as the Present producer does.
 */

/* DXGK_RENDERKM_OPERATION, d3dkmddi.h line 605. */
#define BC250_GDIOP_BITBLT         1u
#define BC250_GDIOP_COLORFILL      2u
#define BC250_GDIOP_ALPHABLEND     3u
#define BC250_GDIOP_STRETCHBLT     4u
#define BC250_GDIOP_ESCAPE         5u
#define BC250_GDIOP_TRANSPARENTBLT 6u
#define BC250_GDIOP_CLEARTYPEBLEND 7u

/* D3DKMDT_GDISURFACETYPE, d3dkmdt.h line 1380, for the types this driver admits. Types 5..8 are
 * refused at CreateAllocation (gdi_private.h, WddmGdiAllocationPolicy), so they cannot appear in
 * an allocation list; the admission below still names them rather than trust that. */
#define BC250_GDISURFACE_TEXTURE            1u
#define BC250_GDISURFACE_STAGING_CPUVISIBLE 2u
#define BC250_GDISURFACE_STAGING            3u
#define BC250_GDISURFACE_LOOKUPTABLE        4u

/* Wingdi.h stretch modes. HALFTONE and STRETCH_HALFTONE "will never be set" (the STRETCHBLT
 * Remarks); BLACKONWHITE and WHITEONBLACK arrive only for a driver that set
 * SupportMonoStretchBltModes, which this one does not. 0 is what a command that carries no mode
 * leaves behind and is read as COLORONCOLOR. */
#define BC250_GDI_STRETCH_BLACKONWHITE 1u
#define BC250_GDI_STRETCH_WHITEONBLACK 2u
#define BC250_GDI_STRETCH_COLORONCOLOR 3u

/* The gamma lookup allocation's fixed shape, from the CLEARTYPEBLEND GammaSurfAllocationIndex
 * field: "8 bits per pixel, and the resolution is 512 x 16 pixels. Each row ... contains two
 * tables: gamma and inverse gamma. Each table has 256 entries." The entry count itself is
 * BC250_GDI_GAMMA_ENTRIES of gdi_pixel.h, which the kernels read too. */
#define BC250_GDI_GAMMA_WIDTH   512u
#define BC250_GDI_GAMMA_HEIGHT  16u

typedef struct BC250_GDI_RECT {
    int Left, Top, Right, Bottom;
} BC250_GDI_RECT;

/* One allocation of the command's allocation list, as this driver knows it: its LB7A record plus
 * the channel order its D3DDDIFORMAT implies. Pitch and Bytes are the allocation's own; the DDI
 * may override the pitch for the two CPU-visible types, which Bc250GdiEffectivePitch resolves. */
typedef struct BC250_GDI_SURFACE {
    unsigned int Width, Height, Pitch, Bpp;     /* Bpp 4 for the colour formats, 1 for A8 */
    unsigned long long Bytes;
    unsigned int RedFirst;                      /* 1 for A8B8G8R8 and X8B8G8R8 */
    unsigned int Type;                          /* BC250_GDISURFACE_* */
    unsigned int Present;                       /* 0 when the allocation-list slot is empty */
} BC250_GDI_SURFACE;

/* A DXGK_RENDERKM_COMMAND with its union flattened: every field of all six DXGK_GDIARG_*
 * structures, named as the DDI names them. wddm.c fills only the members the opcode has and zeroes
 * the rest, so an unset member is 0 here and the admission below says which ones it reads. */
typedef struct BC250_GDI_OP {
    unsigned int OpCode;
    BC250_GDI_RECT SrcRect, DstRect;
    unsigned int SrcAllocationIndex, DstAllocationIndex;
    unsigned int TmpSurfAllocationIndex, GammaSurfAllocationIndex, AlphaSurfAllocationIndex;
    unsigned int NumSubRects;
    unsigned int Rop, Rop3;                     /* BITBLT and COLORFILL */
    /* A8R8G8B8, as the DDI reference documents all three Color fields ("in 32-bit ARGB unsigned
     * pixel format (as defined by the D3DDDIFMT_A8R8G8B8 value)"). One contradiction is recorded
     * and not resolved here: d3dkmddi.h's trailing comment on DXGK_GDIARG_COLORFILL.Color says
     * "Color in the format of the destination surface" instead. The two readings differ only for a
     * red-first destination (A8B8G8R8, X8B8G8R8), which no CDD surface seen so far is; the caller
     * follows the page, converts Color once per command into the destination's channel order, and
     * counts the red-first case so that the lab can say whether it ever happens. */
    unsigned int Color, Color2;
    unsigned int Gamma;                         /* row index, or BC250_GDI_INVALID_GAMMA */
    int DstToAlphaOffsetX, DstToAlphaOffsetY;
    unsigned int SourceConstantAlpha, SourceHasAlpha;   /* ALPHABLEND */
    unsigned int HonorAlpha;                    /* TRANSPARENTBLT, D3DKM_TRANSPARENTBLTFLAGS */
    unsigned int Mode, MirrorX, MirrorY;        /* STRETCHBLT */
    unsigned int SrcPitch, DstPitch, AlphaSurfPitch;    /* the DDI's own pitch members */
} BC250_GDI_OP;

/* Why a command was refused. The mapping to an NTSTATUS is wddm.c's and follows the
 * DXGKDDI_RENDERKM return table: a parameter the hardware cannot support is
 * STATUS_INVALID_PARAMETER, a bad allocation-list index or an absent allocation is
 * STATUS_INVALID_HANDLE, and anything structurally wrong in the buffer is
 * STATUS_ILLEGAL_INSTRUCTION. A format pair that would need a channel conversion the DDI cannot
 * ask for is STATUS_GRAPHICS_CANNOTCOLORCONVERT, as the Present producer already answers. */
enum bc250_gdi_refusal {
    Bc250GdiOk = 0,
    Bc250GdiSkip,           /* DXGK_GDIOP_ESCAPE: "Driver ignores this command" */
    Bc250GdiBadOpCode,
    Bc250GdiBadRop,         /* INVALID, or ROP3 without SupportAllBltRops */
    Bc250GdiBadMode,        /* a stretch mode or mirror bit this driver did not advertise */
    Bc250GdiBadIndex,       /* an allocation-list index out of range, or an absent allocation */
    Bc250GdiBadSurface,     /* geometry, pitch or type the driver cannot address */
    Bc250GdiBadFormat,      /* source and destination channel order differ */
    Bc250GdiBadRect,        /* a rectangle that is not well ordered, or empty where it may not be */
    Bc250GdiBadGamma,       /* the gamma row or the lookup allocation's shape */
    Bc250GdiBadSameSurface, /* a case the No*SameBitmap* caps said would not be asked for */
    Bc250GdiRefusals
};

/* What the command stream does with an admitted command. COPY and FILL are exactly representable
 * by the CP's DMA_DATA engine and are byte operations with no arithmetic, so they are emitted
 * directly; everything else needs a per-pixel kernel. */
enum bc250_gdi_work {
    Bc250GdiWorkNone = 0,
    Bc250GdiWorkCopy,       /* BITBLT, SRCCOPY, equal extents, one channel order: DMA_DATA copy */
    Bc250GdiWorkFill,       /* COLORFILL, PATCOPY: DMA_DATA fill from an immediate dword */
    Bc250GdiWorkKernel      /* a compute dispatch; Kernel says which */
};

/* The kernels, one per arithmetic class. Numbered, not derived from the opcode, because BITBLT and
 * COLORFILL split by ROP and STRETCHBLT shares its kernel with the stretching forms of the other
 * blits. The names are the entry points in gdi_kernels.cl. */
enum bc250_gdi_kernel {
    Bc250GdiKernelNone = 0,
    Bc250GdiKernelBitBltRop,        /* gdi_bitblt_rop:   SRCINVERT/SRCAND/SRCOR, equal extents */
    Bc250GdiKernelColorFillRop,     /* gdi_colorfill_rop */
    Bc250GdiKernelAlphaBlend,       /* gdi_alphablend,     with or without stretch */
    Bc250GdiKernelStretchBlt,       /* gdi_stretchblt:     a point resample, any ROP-free copy */
    Bc250GdiKernelTransparentBlt,   /* gdi_transparentblt, with or without stretch */
    Bc250GdiKernelClearTypeBlend,   /* gdi_cleartypeblend */
    Bc250GdiKernels
};

/* One admitted sub-rectangle, ready to be turned into packets or dispatched. Offsets are byte
 * offsets inside their allocations and are proved to lie inside Bytes for every row the item
 * names, including the last.
 *
 * The source mapping is held as the four extents of the contract's own formula rather than as a
 * step, because the formula is exact rational arithmetic and a fixed-point step is not:
 *     Xs = truncate((Xd - DstRect.left + 0.5) * Ws/Wd + SrcRect.left)
 *        = SrcLeft + (2*(Xd - DstLeft0) + 1) * Ws / (2*Wd)       (integers, exact)
 * where DstLeft0 is the ORIGINAL DstRect.left and not this sub-rectangle's left edge, so a
 * clipped sub-rectangle samples the same source pixels it would have without the clip.
 * Bc250GdiSourceX/Y evaluate it; the host test checks them against a double-precision oracle.
 */
typedef struct BC250_GDI_ITEM {
    unsigned int DstLeft, DstTop, DstWidth, DstHeight;
    unsigned long long DstOffset;        /* byte offset of (DstLeft, DstTop) */
    unsigned int DstPitch;

    unsigned long long SrcBase;          /* byte offset of the source surface origin (0,0) */
    unsigned int SrcPitch;
    int DstLeft0, DstTop0;               /* the command's DstRect origin */
    int SrcLeft, SrcTop;                 /* the command's SrcRect origin */
    unsigned int Ws, Wd, Hs, Hd;         /* the command's source and destination extents */
    unsigned int Stretch;                /* 0 when Ws==Wd and Hs==Hd: a plain translation */
    /* The translated source origin and the row stride of the copy, valid when Stretch is 0. */
    unsigned long long SrcOffset;
    unsigned int RowBytes;

    /* ClearTypeBlend only. */
    unsigned long long AlphaOffset;      /* byte offset of the alpha pixel under (DstLeft, DstTop) */
    unsigned int AlphaPitch;
    unsigned long long GammaOffset;      /* byte offset of the selected gamma row */
    unsigned int GammaValid;

    unsigned int Work;                   /* bc250_gdi_work */
    unsigned int Kernel;                 /* bc250_gdi_kernel */
} BC250_GDI_ITEM;

/* Whether this driver image can perform EVERY operation of DXGK_RENDERKM_OPERATION, which is what
 * DXGK_PRESENTATIONCAPS.SupportKernelModeCommandBuffer promises: the structure has no "cannot" bit
 * for an individual operation, so a driver that sets the bit is sent all seven and DxgkDdiRenderGdi
 * has no way to refuse one (its return table bugchecks on any status but STATUS_SUCCESS).
 *
 * The answer is the presence of the six gfx1013 kernels of gdi_kernels.cl in this driver image, so
 * the function lives beside them in gdi_dispatch.c and reads the generated table rather than a
 * constant: an image that lost a kernel answers 0, the capability bit stays clear, and the caps
 * answer is byte for byte what the release before this one gave. It is a function and not a #if so
 * that the gate, the capability and the log line all read the same fact at a start. */
int Bc250GdiKernelsPresent(void);

/* Whether an opcode reads a source allocation at all: BITBLT, ALPHABLEND, STRETCHBLT and
 * TRANSPARENTBLT do, COLORFILL, ESCAPE and CLEARTYPEBLEND do not. Exported because the caller
 * resolves the allocation-list slots a command names and must agree with the admission about which
 * ones those are. 0 for an opcode outside the table. */
int Bc250GdiOpHasSource(unsigned int OpCode);

/* The pitch that addresses a surface in this command: the DDI's member for the two CPU-visible
 * types, the allocation's own for every other type. "Pitch should be ignored for other allocation
 * types" (the BITBLT Remarks). 0 means the pair is not addressable and the caller refuses. */
unsigned int Bc250GdiEffectivePitch(const BC250_GDI_SURFACE* Surface, unsigned int DdiPitch);

/* Admission of one command against its allocation list. Surfaces is indexed by allocation-list
 * index and Count is AllocationListSize. Nothing is written on a refusal. */
int Bc250GdiAdmitOp(const BC250_GDI_OP* Op, const BC250_GDI_SURFACE* Surfaces, unsigned int Count);

/* The plan of one sub-rectangle of an admitted command. SubRect is in destination coordinates and
 * must lie inside the destination surface, which the DDI guarantees and this checks. Returns a
 * bc250_gdi_refusal; Bc250GdiOk with Item->DstWidth 0 means the rectangle is empty after clipping
 * to DstRect, which is not an error and emits nothing. */
int Bc250GdiPlanItem(const BC250_GDI_OP* Op, const BC250_GDI_SURFACE* Surfaces, unsigned int Count,
                     const BC250_GDI_RECT* SubRect, BC250_GDI_ITEM* Item);

/* The contract's destination-to-source transform, exactly. Xd and Yd are absolute destination
 * coordinates. Both are total: with Ws==Wd they reduce to the translation the BITBLT Remarks give. */
int Bc250GdiSourceX(const BC250_GDI_ITEM* Item, int Xd);
int Bc250GdiSourceY(const BC250_GDI_ITEM* Item, int Yd);

/* The command-buffer walk's arithmetic, apart from the field reads, so that the one place a
 * malformed buffer can overrun is host-tested. CommandBytes is the current command's CommandSize
 * and Offset its start; *Next is Offset + CommandBytes. 0 means the buffer does not hold this
 * command: a size below the minimum, a size that is not a multiple of four, or an end past
 * BufferBytes. MinBytes is the size of a DXGK_RENDERKM_COMMAND's fixed part, which wddm.c passes
 * from the WDK's own sizeof. */
int Bc250GdiWalkStep(unsigned int BufferBytes, unsigned int Offset, unsigned int CommandBytes,
                     unsigned int MinBytes, unsigned int* Next);

/* MultipassOffset, packed as the Supporting Kernel-Mode Command Buffers page suggests: the high 16
 * bits are the index of the command being translated and the low 16 bits the number of
 * sub-rectangles of it already done. A command with more than 65535 sub-rectangles, or a buffer
 * with more than 65535 commands, cannot be resumed and is refused rather than silently truncated.
 */
#define BC250_GDI_PASS_MAX 0xFFFFu
unsigned int Bc250GdiPackPass(unsigned int Command, unsigned int SubRect);
unsigned int Bc250GdiPassCommand(unsigned int Pass);
unsigned int Bc250GdiPassSubRect(unsigned int Pass);

#endif
