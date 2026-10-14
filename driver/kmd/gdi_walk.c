/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The walk of one GDI command buffer. gdi_walk.h states the order and why it is the contract.
 *
 * This file holds the only code of the driver that reads the operating system's command buffer. It
 * needs the WDK declaration of that buffer and nothing else of the kernel, which is what lets
 * run_gdi_walk.ps1 compile it as it is and run it against an inaccessible page. That test supplies
 * the declarations itself; gdi_walk_ddi.h says how.
 */
#include "gdi_walk_ddi.h"
#include "gdi_walk.h"

/* The fixed part of a command, and the size of the member each opcode names. Both come from the
 * WDK's own layout. A member is never read here, only measured. */
#define GDI_HEADER_BYTES ((unsigned int)FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command))
#define GDI_MEMBER_BYTES(m) ((unsigned int)sizeof(((const DXGK_RENDERKM_COMMAND*)0)->Command.m))

/* The producer builds these commands as its own fixed part plus 16 bytes for each sub-rectangle.
 * cdd.dll 10.0.22621.6630 and 10.0.26100.9444 use 0x30 for ColorFill, 0x48 for AlphaBlend,
 * TransparentBlt and StretchBlt, and 0x50 for BitBlt and ClearTypeBlend (see
 * docs/design/gdi-hw-accel.md, "How far the bound is from the ordinary producer"). The sizes this
 * driver computes from the WDK union are the same six numbers, which is what makes MinBytes the
 * producer's fixed part and not a guess. A WDK layout change that breaks the equality breaks the capacity
 * arithmetic with it, so it stops the build here. */
C_ASSERT(GDI_HEADER_BYTES == 8u);
C_ASSERT(GDI_HEADER_BYTES + GDI_MEMBER_BYTES(ColorFill) == 0x30u);
C_ASSERT(GDI_HEADER_BYTES + GDI_MEMBER_BYTES(AlphaBlend) == 0x48u);
C_ASSERT(GDI_HEADER_BYTES + GDI_MEMBER_BYTES(TransparentBlt) == 0x48u);
C_ASSERT(GDI_HEADER_BYTES + GDI_MEMBER_BYTES(StretchBlt) == 0x48u);
C_ASSERT(GDI_HEADER_BYTES + GDI_MEMBER_BYTES(BitBlt) == 0x50u);
C_ASSERT(GDI_HEADER_BYTES + GDI_MEMBER_BYTES(ClearTypeBlend) == 0x50u);
/* The sub-rectangle array is an array of RECT, which is the unit the capacity arithmetic counts in
 * (BC250_GDI_SUBRECT_BYTES of gdi_batch.h). */
C_ASSERT(sizeof(RECT) == 16u);
C_ASSERT(sizeof(BC250_GDI_RECT) == sizeof(RECT));

unsigned int Bc250GdiOpMinBytes(unsigned int OpCode)
{
    switch (OpCode)
    {
    case DXGK_GDIOP_BITBLT:         return GDI_HEADER_BYTES + GDI_MEMBER_BYTES(BitBlt);
    case DXGK_GDIOP_COLORFILL:      return GDI_HEADER_BYTES + GDI_MEMBER_BYTES(ColorFill);
    case DXGK_GDIOP_ALPHABLEND:     return GDI_HEADER_BYTES + GDI_MEMBER_BYTES(AlphaBlend);
    case DXGK_GDIOP_STRETCHBLT:     return GDI_HEADER_BYTES + GDI_MEMBER_BYTES(StretchBlt);
    case DXGK_GDIOP_TRANSPARENTBLT: return GDI_HEADER_BYTES + GDI_MEMBER_BYTES(TransparentBlt);
    case DXGK_GDIOP_CLEARTYPEBLEND: return GDI_HEADER_BYTES + GDI_MEMBER_BYTES(ClearTypeBlend);
    /* "Driver ignores this command". No member of the union is read, so the fixed part is all this
     * command has to hold. */
    case DXGK_GDIOP_ESCAPE:         return GDI_HEADER_BYTES;
    default:                        return 0u;
    }
}

void Bc250GdiWalkRect(const RECT* From, BC250_GDI_RECT* To)
{
    To->Left = From->left; To->Top = From->top; To->Right = From->right; To->Bottom = From->bottom;
}

/* One command of the buffer, flattened into the plain record the contract layer admits. Only the
 * union member the opcode names is read; the sub-rectangle array and its count come back
 * separately, because they still point into the OS command buffer.
 *   The caller must have established that Bc250GdiOpMinBytes(OpCode) bytes of this command lie
 * inside the command buffer. Bc250GdiWalkCommand is the only caller, and Bc250GdiCommandSize is how
 * it establishes that. Reading a member before that check is the defect of review r1. */
static void Bc250GdiDecodeCommand(const DXGK_RENDERKM_COMMAND* Command, BC250_GDI_OP* Op,
                                  const RECT** SubRects, unsigned int* SubCount)
{
    switch (Command->OpCode)
    {
    case DXGK_GDIOP_BITBLT:
        Bc250GdiWalkRect(&Command->Command.BitBlt.SrcRect, &Op->SrcRect);
        Bc250GdiWalkRect(&Command->Command.BitBlt.DstRect, &Op->DstRect);
        Op->SrcAllocationIndex = Command->Command.BitBlt.SrcAllocationIndex;
        Op->DstAllocationIndex = Command->Command.BitBlt.DstAllocationIndex;
        Op->Rop = Command->Command.BitBlt.Rop;
        Op->Rop3 = Command->Command.BitBlt.Rop3;
        Op->SrcPitch = Command->Command.BitBlt.SrcPitch;
        Op->DstPitch = Command->Command.BitBlt.DstPitch;
        *SubRects = Command->Command.BitBlt.pSubRects;
        *SubCount = Command->Command.BitBlt.NumSubRects;
        break;
    case DXGK_GDIOP_COLORFILL:
        Bc250GdiWalkRect(&Command->Command.ColorFill.DstRect, &Op->DstRect);
        Op->DstAllocationIndex = Command->Command.ColorFill.DstAllocationIndex;
        Op->Color = Command->Command.ColorFill.Color;
        Op->Rop = Command->Command.ColorFill.Rop;
        Op->Rop3 = Command->Command.ColorFill.Rop3;
        *SubRects = Command->Command.ColorFill.pSubRects;
        *SubCount = Command->Command.ColorFill.NumSubRects;
        break;
    case DXGK_GDIOP_ALPHABLEND:
        Bc250GdiWalkRect(&Command->Command.AlphaBlend.SrcRect, &Op->SrcRect);
        Bc250GdiWalkRect(&Command->Command.AlphaBlend.DstRect, &Op->DstRect);
        Op->SrcAllocationIndex = Command->Command.AlphaBlend.SrcAllocationIndex;
        Op->DstAllocationIndex = Command->Command.AlphaBlend.DstAllocationIndex;
        Op->SourceConstantAlpha = Command->Command.AlphaBlend.SourceConstantAlpha;
        Op->SourceHasAlpha = Command->Command.AlphaBlend.SourceHasAlpha ? 1u : 0u;
        Op->SrcPitch = Command->Command.AlphaBlend.SrcPitch;
        *SubRects = Command->Command.AlphaBlend.pSubRects;
        *SubCount = Command->Command.AlphaBlend.NumSubRects;
        break;
    case DXGK_GDIOP_STRETCHBLT:
        Bc250GdiWalkRect(&Command->Command.StretchBlt.SrcRect, &Op->SrcRect);
        Bc250GdiWalkRect(&Command->Command.StretchBlt.DstRect, &Op->DstRect);
        Op->SrcAllocationIndex = Command->Command.StretchBlt.SrcAllocationIndex;
        Op->DstAllocationIndex = Command->Command.StretchBlt.DstAllocationIndex;
        /* Read as bitfields, never from a transcribed mask: the union's Flags word holds Mode in
         * its low 16 bits and the two mirror bits above them. */
        Op->Mode = Command->Command.StretchBlt.Mode;
        Op->MirrorX = Command->Command.StretchBlt.MirrorX;
        Op->MirrorY = Command->Command.StretchBlt.MirrorY;
        Op->SrcPitch = Command->Command.StretchBlt.SrcPitch;
        *SubRects = Command->Command.StretchBlt.pSubRects;
        *SubCount = Command->Command.StretchBlt.NumSubRects;
        break;
    case DXGK_GDIOP_TRANSPARENTBLT:
        Bc250GdiWalkRect(&Command->Command.TransparentBlt.SrcRect, &Op->SrcRect);
        Bc250GdiWalkRect(&Command->Command.TransparentBlt.DstRect, &Op->DstRect);
        Op->SrcAllocationIndex = Command->Command.TransparentBlt.SrcAllocationIndex;
        Op->DstAllocationIndex = Command->Command.TransparentBlt.DstAllocationIndex;
        Op->Color = Command->Command.TransparentBlt.Color;
        Op->HonorAlpha = Command->Command.TransparentBlt.Flags.HonorAlpha;
        Op->SrcPitch = Command->Command.TransparentBlt.SrcPitch;
        *SubRects = Command->Command.TransparentBlt.pSubRects;
        *SubCount = Command->Command.TransparentBlt.NumSubRects;
        break;
    case DXGK_GDIOP_CLEARTYPEBLEND:
        Bc250GdiWalkRect(&Command->Command.ClearTypeBlend.DstRect, &Op->DstRect);
        Op->DstAllocationIndex = Command->Command.ClearTypeBlend.DstAllocationIndex;
        Op->TmpSurfAllocationIndex = Command->Command.ClearTypeBlend.TmpSurfAllocationIndex;
        Op->GammaSurfAllocationIndex = Command->Command.ClearTypeBlend.GammaSurfAllocationIndex;
        Op->AlphaSurfAllocationIndex = Command->Command.ClearTypeBlend.AlphaSurfAllocationIndex;
        Op->DstToAlphaOffsetX = Command->Command.ClearTypeBlend.DstToAlphaOffsetX;
        Op->DstToAlphaOffsetY = Command->Command.ClearTypeBlend.DstToAlphaOffsetY;
        Op->Color = Command->Command.ClearTypeBlend.Color;
        Op->Color2 = Command->Command.ClearTypeBlend.Color2;
        Op->Gamma = Command->Command.ClearTypeBlend.Gamma;
        Op->AlphaSurfPitch = Command->Command.ClearTypeBlend.AlphaSurfPitch;
        *SubRects = Command->Command.ClearTypeBlend.pSubRects;
        *SubCount = Command->Command.ClearTypeBlend.NumSubRects;
        break;
    /* An escape carries nothing, and an opcode outside the table never reaches this function. */
    default:
        break;
    }
    Op->NumSubRects = *SubCount;
}

int Bc250GdiWalkCommand(const unsigned char* Buffer, unsigned int Bytes, unsigned int Offset,
                        BC250_GDI_OP* Op, const RECT** SubRects, unsigned int* SubCount,
                        unsigned int* Next, unsigned int* Why)
{
    const DXGK_RENDERKM_COMMAND* command;
    BC250_GDI_CMD_VIEW view;
    unsigned int minBytes;
    int kind;

    if (!Buffer || !Op || !SubRects || !SubCount || !Next || !Why) return BC250_GDI_CMD_STOP;
    *Next = 0;
    *Why = BC250_GDI_WHY_MALFORMED;
    *SubRects = NULL;
    *SubCount = 0;
    RtlZeroMemory(Op, sizeof(*Op));

    /* Step 1. The header holds the two fields read below. The caller's loop admits a command only
     * while they fit, and this says so again, because the proof of every read behind it starts
     * here. */
    if (!Bc250GdiHeaderFits(Bytes, Offset, GDI_HEADER_BYTES)) return BC250_GDI_CMD_STOP;
    command = (const DXGK_RENDERKM_COMMAND*)(Buffer + Offset);

    /* Step 2. The opcode alone gives the fixed size. An opcode outside the table stops the walk:
     * its length is unknown, so the command behind it cannot be found. */
    Op->OpCode = (unsigned int)command->OpCode;
    minBytes = Bc250GdiOpMinBytes(Op->OpCode);
    if (minBytes == 0u) { *Why = BC250_GDI_WHY_OPCODE; return BC250_GDI_CMD_STOP; }

    /* Step 3. CommandSize against the fixed size and against the rest of the buffer. */
    RtlZeroMemory(&view, sizeof(view));
    view.BufferBytes = Bytes;
    view.Offset = Offset;
    view.CommandBytes = command->CommandSize;
    view.MinBytes = minBytes;
    view.HeaderBytes = GDI_HEADER_BYTES;
    view.RectBytes = (unsigned int)sizeof(RECT);
    view.Base = (unsigned long long)(ULONG_PTR)Buffer;
    if (!Bc250GdiCommandSize(&view, Next, Why)) return BC250_GDI_CMD_STOP;

    /* Step 4. The whole fixed part is inside the buffer now, so the member may be read. */
    Bc250GdiDecodeCommand(command, Op, SubRects, SubCount);
    view.SubCount = *SubCount;
    view.First = (unsigned long long)(ULONG_PTR)*SubRects;
    view.Escape = Op->OpCode == BC250_GDIOP_ESCAPE;
    view.HaveRects = *SubRects != NULL;
    kind = Bc250GdiCommandRects(&view, Why);
    return kind;
}
