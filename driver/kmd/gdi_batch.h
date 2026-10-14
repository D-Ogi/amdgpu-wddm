/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_BATCH_H
#define BC250_GDI_BATCH_H
#include "gdi_render.h"

/*
 * The capacity contract of one DxgkDdiRenderGdi call, and the choice of path for each admitted
 * sub-rectangle.
 *
 * The guaranteed contract DMA buffer model gives one DMA buffer for one command buffer and no
 * second pass: "The display miniport driver must be able to translate one command buffer into
 * exactly one DMA buffer and one patch-location list. If this translation is not possible, the
 * user-mode command buffer is, by definition, invalid. The display miniport driver cannot return
 * status that indicates it is out of DMA buffer space"
 * (display/using-the-guaranteed-contract-dma-buffer-model.md, ref revision 110f60ea). RenderGdi
 * also has no status for it: its return table is STATUS_SUCCESS, and "All other return values will
 * lead to the OS bugcheck" (ref/ddi-display/d3dkmddi.md, DXGKDDI_RENDERGDI).
 *
 * So capacity cannot be a per-item question. If it were, an early item with a tall rectangle would
 * spend the buffer on its own rows and a later item would be dropped from a call that reported
 * success, which is what a review of increment 1 reproduced on this host: a 2 by 8192 ColorFill
 * with two 1 by 8192 sub-rectangles filled the first column in 57344 dwords and lost the second.
 *
 * Three rules remove that class:
 *
 *   1. Every admitted sub-rectangle has a bounded cost, whatever its size: one compute dispatch of
 *      the kernel of its operation, which is a fixed number of dwords. The per-row DMA_DATA path
 *      is cheaper for a short rectangle and unbounded for a tall one, so it is an optimisation and
 *      never an obligation. An item whose rows do not fit the dwords left over after the bounded
 *      cost of everything behind it takes the dispatch instead.
 *   2. The whole batch is checked before anything is written. Bc250GdiBatchBegin takes the number
 *      of sub-rectangles the command buffer holds and refuses the call if their bounded cost, the
 *      cache operation, the code block and the ring padding do not fit. The caller then writes an
 *      empty DMA buffer, which draws nothing and leaves nothing half drawn.
 *   3. The capacity those two rules work inside is not the DMA buffer alone. It is the smallest of
 *      the space dxgkrnl reports for the call, the room the call's own buffer has left behind that
 *      window, the length one indirect buffer can carry, and that length less the dwords this
 *      driver has already written into the same DMA buffer: Bc250GdiCallCapacity over the room the
 *      claim below answers with. The row path of rule 1 is offered a budget inside that capacity,
 *      so a larger DMA buffer cannot grant it space the submitter is unable to encode. The last two
 *      terms are established for the buffer the call lies in, by the tracker below, from that
 *      buffer's recorded extent and never from the order or the distance of addresses.
 *
 * Rule 1 is what makes rule 2 a contract rather than a hope: after Begin has said yes, no admitted
 * sub-rectangle can be refused for want of space, and the host test asserts exactly that over a
 * set of batches that includes the counterexample above.
 *
 * Rule 3 is the ceiling the hardware puts on all of it, and it is the one that binds. Everything
 * this driver writes into a GDI context's DMA buffer is submitted as a single indirect buffer
 * (GfxSubmitIb, and bc250_gfx_emit_ib under it). The length field of that packet is 20 bits wide,
 * so 1048575 dwords is the longest stream the ring can be given at all, and the batch pads to the
 * eight-dword ring granule, which leaves BC250_GDI_IB_DWORDS_MAX. A longer stream is not truncated
 * and not drawn in part: the emitter refuses the submission whole, and a refused nonempty
 * submission stays outstanding for recovery. Nothing here would see that happen, which is why this
 * is a bound on what may be written and not a check at the end: the dwords are never allocated.
 * The constant is held against AMD's own field definition by C_ASSERT in gdi_dispatch.c.
 *
 * The last two terms of rule 3 are a question about identity, and the DDI answers it with an
 * address and a length. DXGKARG_RENDERGDI names the buffer of a call by pDmaBuffer, "a pointer to
 * the start of the DMA buffer, which is aligned on 4 KB", by DmaBufferGpuVirtualAddress, "the
 * virtual address where the DMA buffer was paged in", and by DmaSize, "the size, in bytes, of the
 * DMA buffer that pDmaBuffer points to" (ref/ddi-display/d3dkmddi.md, DXGKARG_RENDERGDI, WDK
 * 10.0.26100). The same two fields of DXGKARG_RENDER are declared [out] and [in] in the same file,
 * with the driver's own obligation spelled out: "the driver should set pDmaBuffer to the next empty
 * byte that follows the last byte that the driver wrote to". So what a call is given is a window
 * and the extent it lies in, and that pair is the only identity on offer. It does not promise that
 * one DMA buffer carries one call, and it does not promise any order of RenderGdi calls and
 * submissions. Neither does the general submission guide, which separates the translation from the
 * scheduler's submission.
 *
 * A buffer is therefore identified by its extent and by nothing else. Adjacency is not identity: a
 * window that begins exactly where our output in one buffer ended is just as likely to be another
 * buffer the memory manager placed next to it, with its own full size, and review r5 built that
 * case. So the tracker holds the extent of every buffer this context has unsubmitted output in,
 * with a generation each call's claim carries:
 *
 *   Bc250GdiDmaClaim places the window a call was given inside those extents. A window inside an
 *   extent is that buffer, and then the dwords ahead of the call must be exactly the dwords we
 *   wrote into it, or the window is a position this driver cannot explain and the call is refused
 *   as a whole. A window inside no extent is a buffer this context holds no output in, which by the
 *   4 KB sentence above is a buffer start, and nothing of ours is ahead of it. The claim reserves
 *   its slot until the call publishes or gives it back, so a second call cannot take a slot whose
 *   length is still being decided, and the room it answers with is the room that buffer has left
 *   behind the window, which is what keeps a continuation inside the extent it belongs to.
 *
 *   Bc250GdiDmaPublish records what a call wrote, against the generation of the claim it holds, so
 *   output can never be recorded into a slot that was released and taken again in between.
 *   Bc250GdiDmaAbort gives back a claim that wrote nothing, which every refused call does.
 *   Bc250GdiDmaSubmitted releases the buffers one submission carried away, by the address range of
 *   that submission, so a submission of one buffer cannot clear the state of another, and it leaves
 *   a reserved slot alone: a call still deciding its own length is not a buffer anyone can retire.
 *
 * Nothing but this driver writes hardware commands into a GDI context's DMA buffer, so what the
 * tracker holds is the whole of what that buffer contains, which is what makes a continuation exact
 * rather than estimated. A window nobody can place and a context with no slot left are both the
 * whole call dropped and counted, because a prefix nobody can establish is not a prefix anyone may
 * assume to be zero. The number of buffers one context can hold at a time is BC250_GDI_DMA_SLOTS,
 * and a context that would need more refuses the call instead of forgetting a buffer it has written
 * into or stealing the slot of a call in flight.
 *
 * With rule 3 in place, rule 2's refusal is unreachable for every command buffer up to a bound,
 * and the DDI gives the input bound its arithmetic:
 *
 *   "CommandSize: The size of the current command, in bytes. This is equal to the number of bytes
 *    from the beginning of DXGK_RENDERKM_COMMAND up to the next command."
 *   "Each command varies in length depending on the value of the OpCode member and the number of
 *    sub-rectangles in the command."
 *      (ref/ddi-display/d3dkmddi.md:27128 and :27169, DXGK_RENDERKM_COMMAND, WDK 10.0.26100)
 *
 * A command's length can depend on how many sub-rectangles it has only if they are inside it, and
 * CommandSize reaches to the next command, so the array of one command lies inside that command and
 * the arrays of different commands do not overlap. gdi_walk.c enforces exactly that reading and
 * drops a pSubRects that points outside the command it belongs to. An array may overlap its own
 * command's fixed part, which one producer does, so the floor of the count is the eight-byte header.
 * Therefore, writing L for CommandLength:
 *
 *     sum over commands of 16 * NumSubRects <= L - 8,  so  N <= (L - 8) / 16
 *
 * and with the bounded cost of rule 1 the whole call needs at most
 *
 *     acquire + codeMax + ((L - 8) / 16) * dispatchMax + granule - 1   dwords,
 *
 * which is Bc250GdiCallDwords(L). It is a constant factor of the input: 324 bytes of DMA buffer per
 * 16 bytes of command buffer, 20.25 to one. So a GDI context whose DMA buffer is
 * Bc250GdiContextDmaBytes(bound) bytes translates every command buffer up to bound, and the refusal
 * in Begin is then unreachable for that whole input domain, not merely for the inputs anyone has
 * tried.
 *
 * The DDI does not publish the size of the command buffer dxgkrnl gives a GDI context, and nothing
 * in this driver can make the operating system tell it: the return table has no out-of-space status,
 * so there is no call to repeat with a larger buffer. The bound is therefore ours, and it is as far
 * from any producer as one indirect buffer reaches:
 *
 *   BC250_GDI_CMDBUF_BOUND is the largest candidate on this driver's own ladder of command-buffer
 *   sizes, which is BC250_GDI_CMDBUF_FLOOR doubled while it still fits, whose worst case fits both
 *   ceilings: BC250_GDI_IB_DWORDS_MAX, what one indirect buffer can carry, and
 *   BC250_GDI_DMA_BYTES_MAX, the size ceiling this driver puts on one GDI context's DMA buffer.
 *
 * That is 192 KB of command buffer, 12287 guaranteed sub-rectangles and 3.8 MB of DMA buffer. The
 * next candidate, 384 KB, needs 1991934 dwords, which no single indirect buffer can carry, and the
 * host test asserts both halves of that statement. The hardware ceiling is what chooses the bound
 * today; the DMA ceiling is a policy limit that lies well above it.
 *
 * The bound does not depend on which producer filled the buffer, and it is three times the whole
 * command buffer of the ordinary one. cdd.dll allocates 65536 bytes and submits what it has written
 * of it (docs/design/gdi-hw-accel.md, "How far the bound is from the ordinary producer", which names
 * the builds that were read). The diagnostic producer inside dxgkrnl supplies a command buffer of
 * its own whose length nothing documents or validates beyond "not zero", so no bound read off a
 * producer would cover it, and this one covers everything that producer can supply up to 192 KB.
 *
 * Above the bound the behaviour is defined, counted and visible, and it is not drawing. The bound
 * is where the guarantee ends, not where the refusal starts: Begin is given the sub-rectangles the
 * walk actually counted, so a sparse call whose own work fits is translated whole however long its
 * command buffer is, and a call whose counted work does not fit the capacity is refused as a whole.
 * The guarantee is a statement about a whole DMA buffer, so a command buffer inside the bound can
 * still be refused when the buffer it was given holds an earlier call of the same buffer: the
 * capacity of rule 3, and not the length of the command buffer alone, is what Begin is asked
 * about. GdiRenderCapacityShort counts the calls whose worst case was larger than the capacity they
 * were given, GdiRenderBatchRefused every call refused as a whole, GdiRenderOmittedSubRects the
 * sub-rectangles lost with them, and the guard log names the length, the capacity and the dwords
 * ahead of each. A refusal cannot repair the call it happened on, and it cannot resize a context
 * that already exists. There is no honest alternative inside this DDI: a prefix of the call is a
 * half-drawn screen, and any status other than success bugchecks Windows. Every one of these
 * refusals is work the capability admitted and did not draw, which is the limitation
 * docs/design/gdi-hw-accel.md states and nothing here claims to have closed.
 *
 * The size asked for is bounded a second time, by policy: Bc250GdiContextBound steps the bound down
 * until the DMA buffer is at most BC250_GDI_DMA_APERTURE_SHARE of the aperture this start was
 * granted, so one GDI context cannot eat the segment its own surfaces live in. That is a policy
 * constant and not a proof that the segment can afford the buffer, which no arithmetic here could
 * establish: concurrent contexts, their buffers and every other allocation compete for the same
 * segment. At every aperture this driver accepts, 256 MB and up, the share is 32 MB or more and the
 * whole bound fits, so the step-down does not reduce the bound anywhere in that range; it is there
 * for the smaller apertures a future start might report. The capacity of each call is taken from the
 * space that call reports, whatever that is, so nothing here depends on the size asked for being
 * granted.
 *
 * The DMA buffer "can grow and shrink after the context is created; however, the DMA buffer can
 * never shrink smaller than the starting size that DmaBufferSize specifies" (DXGK_CONTEXTINFO), so
 * the size asked for is a floor the operating system undertakes to keep. GdiRenderDmaShort is the
 * check of that sentence and not a way of handling an allocation failure: it counts the calls that
 * were given the start of a buffer and reported less space than this context asked for, which the
 * contract says cannot happen, and a call that continues inside a buffer is never counted there,
 * because the space it reports is the window it has left and not the size of the buffer. This
 * implementation has no RenderGdi continuation and no on-demand enlargement: it neither asks for a
 * larger buffer in the middle of a call nor splits a call across buffers, and dxgkrnl may fill one
 * DMA buffer with several calls before submitting it, which is why rule 3 subtracts what is already
 * there.
 *
 * This file is plain C over plain integers, like gdi_render.c, so the contract is tested on this PC
 * and not on the lab. Residency, cache state and the addresses themselves stay the caller's
 * obligation.
 */

/* The ring takes a 32-byte aligned indirect buffer, which is eight dwords. */
#define BC250_GDI_RING_GRANULE 8u

/* What one indirect buffer can carry. PACKET3_INDIRECT_BUFFER's length field is 20 bits
 * (driver/amdgpu-import/nvd.h, PACKET3_INDIRECT_BUFFER__IB_SIZE), so 1048575 dwords is the longest
 * stream bc250_gfx_emit_ib() accepts; this is that number rounded down to the granule above, which
 * is the length a padded batch can actually reach. gdi_dispatch.c holds it against the AMD macro by
 * C_ASSERT, and the host test checks the same two statements with the macro itself. */
#define BC250_GDI_IB_DWORDS_MAX 1048568u

/* sizeof(RECT) and the smallest DXGK_RENDERKM_COMMAND that can carry a sub-rectangle, which is
 * FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command) + sizeof(DXGK_GDIARG_COLORFILL). Both are held
 * against the WDK headers by C_ASSERT in wddm.c, so this file stays plain C over plain integers. */
#define BC250_GDI_SUBRECT_BYTES 16u
#define BC250_GDI_MIN_COMMAND_BYTES 48u

/* The floor of the count bound, which is the command's header and not its fixed part. A command's
 * sub-rectangle array may overlap its own fixed part: the diagnostic producer inside dxgkrnl builds
 * an 80-byte BitBlt whose pSubRects points at the DstRect inside it (docs/design/gdi-hw-accel.md,
 * "The second producer, and the shape it builds"). Such a command is admitted, because the array
 * lies inside the command, so the budget has to allow it. FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command),
 * held against the WDK by C_ASSERT in gdi_walk.c. */
#define BC250_GDI_RECT_FLOOR_BYTES 8u

/* The size ceiling this driver puts on one GDI context's DMA buffer. It is policy, and it is not
 * what binds: the hardware ceiling of BC250_GDI_IB_DWORDS_MAX stops the bound first. */
#define BC250_GDI_DMA_BYTES_MAX (32u * 1024u * 1024u)

/* The largest command buffer this driver undertakes to translate in one call, and the number the
 * GDI context's DMA buffer size is computed from. It is not a number anyone picked to be "above
 * what the producer sends": it is the largest candidate on the ladder of this driver's own sizes
 * that one indirect buffer can carry, 192 KB of command buffer, which holds at most 12287
 * sub-rectangles and costs 3986424 bytes of DMA buffer. The ordinary producer's whole command
 * buffer is 65536 bytes, three times smaller. The host test proves that the worst case of this
 * bound fits both ceilings, and that the worst case of the next candidate passes the
 * indirect-buffer ceiling although its DMA buffer would have fitted the policy ceiling. */
#define BC250_GDI_CMDBUF_BOUND (192u * 1024u)

/* The smallest bound this driver ever asks for, used when the share below does not admit the one
 * above. It is the bound of tester.30's design and costs 1995768 bytes of DMA buffer, which is
 * inside the per-buffer share of every aperture this driver accepts. */
#define BC250_GDI_CMDBUF_FLOOR (96u * 1024u)

/* The share of the aperture segment a GDI context's DMA buffer may take: policy, as the comment
 * above says, and not a proof that the segment can afford it. The buffer is resident for every
 * submission of that context, and the surfaces it draws into live in the same segment. */
#define BC250_GDI_DMA_APERTURE_SHARE 8u

/* How many DMA buffers of one GDI context this driver can hold at a time, counting the buffer of
 * every call in flight. Every buffer a call writes into takes a slot until a submission carries
 * that buffer away, and a context that would need one more refuses the call rather than forget a
 * buffer it has written into. Eight is a number this driver chose: no trial has reported any of
 * these counters yet, so nothing here says how many buffers one context is really offered, and the
 * ninth is counted and dropped. It is an implementation bound of this driver, not a limit of the
 * DDI, and docs/design/gdi-hw-accel.md states it with the others. */
#define BC250_GDI_DMA_SLOTS 8u

/* Where Bc250GdiDmaClaim placed a call. The first two are the cases the call may be translated in,
 * and the last two refuse it as a whole. */
#define BC250_GDI_DMA_FRESH   0   /* a buffer this context holds no output in: nothing is ahead */
#define BC250_GDI_DMA_APPEND  1   /* inside a buffer we hold output in, exactly where that output ended */
#define BC250_GDI_DMA_UNKNOWN 2   /* the window cannot be placed in any buffer this context holds */
#define BC250_GDI_DMA_FULL    3   /* every slot holds a buffer of ours or a claim in flight */

/* One DMA buffer of this context. Base and Bytes are the extent the DDI gave for it, the address it
 * was paged in at and the size of the buffer that address names, and they are the identity of the
 * buffer: nothing here reads the distance between two buffers. Dwords is what this driver has
 * written into it that no submission has carried away. Generation rises whenever the slot is taken,
 * so a claim cannot record into a slot that was released and taken again. Claimed is a call in
 * flight: its length is not decided yet, so neither another claim nor a submission may take the
 * slot. Base == 0 is a free slot. */
typedef struct BC250_GDI_DMA_SLOT {
    unsigned long long Base;
    unsigned int Bytes;
    unsigned int Dwords;
    unsigned int Generation;
    unsigned int Claimed;
} BC250_GDI_DMA_SLOT;

/* The DMA buffers of one GDI context. Zeroed state is the whole of its initialisation. The caller
 * owns the serialisation: these functions are plain C over the structure and take no lock. */
typedef struct BC250_GDI_DMA_MAP {
    BC250_GDI_DMA_SLOT Slots[BC250_GDI_DMA_SLOTS];
} BC250_GDI_DMA_MAP;

/* What one call holds between its claim and its publication: which slot, which generation of that
 * slot, the dwords of ours ahead of the call in that buffer, and the dwords of room the buffer has
 * left behind the window. Slot == BC250_GDI_DMA_SLOTS is no claim at all, which is what a refused
 * claim leaves, and Bc250GdiDmaPublish and Bc250GdiDmaAbort both ignore it. */
typedef struct BC250_GDI_DMA_CLAIM {
    unsigned int Slot;
    unsigned int Generation;
    unsigned int Ahead;
    unsigned int Room;
} BC250_GDI_DMA_CLAIM;

/* Place the window a call was given and reserve its slot. WindowVa is
 * DXGKARG_RENDERGDI.DmaBufferGpuVirtualAddress, WindowBytes the DmaSize of the same call, and
 * PageBytes the alignment the DDI gives the start of a DMA buffer, which is 4096. *Claim receives
 * the reservation, and the two refusals leave the map unchanged and *Claim empty. A reservation is
 * ended by Bc250GdiDmaPublish or Bc250GdiDmaAbort and by nothing else. Returns one of the four
 * results above. */
int Bc250GdiDmaClaim(BC250_GDI_DMA_MAP* Map, unsigned long long WindowVa, unsigned int WindowBytes,
                     unsigned int PageBytes, BC250_GDI_DMA_CLAIM* Claim);

/* Record the dwords a claimed call wrote, and end the reservation. Returns 0 when the claim does
 * not hold the slot any more, when the record moved under it, or when those dwords would take the
 * buffer past the room of the claim or past what one indirect buffer can carry. A caller that is
 * refused here has not published anything: it must not advance the DDI's own pointer and must count
 * the call as dropped. */
int Bc250GdiDmaPublish(BC250_GDI_DMA_MAP* Map, const BC250_GDI_DMA_CLAIM* Claim,
                       unsigned int Dwords);

/* Give back a reservation that wrote nothing, which every refused call does. A slot that held no
 * output before the claim becomes free again; a slot that held output keeps it. */
void Bc250GdiDmaAbort(BC250_GDI_DMA_MAP* Map, const BC250_GDI_DMA_CLAIM* Claim);

/* Release every buffer of this context inside the address range one submission carried away, which
 * is DXGKARG_SUBMITCOMMANDVIRTUAL.DmaBufferVirtualAddress and DmaBufferSize. Returns how many slots
 * it freed, so a caller can count a submission that matched nothing. A range of zero bytes releases
 * the buffer that begins at that address and nothing else. *Claimed, when given, receives how many
 * buffers inside the range had a call in flight and were therefore left alone. */
unsigned int Bc250GdiDmaSubmitted(BC250_GDI_DMA_MAP* Map, unsigned long long BufferVa,
                                  unsigned int Bytes, unsigned int* Claimed);

/* What Bc250GdiBatchItem did with an item. */
#define BC250_GDI_BATCH_REFUSED  0   /* the item is not addressable; nothing was written */
#define BC250_GDI_BATCH_ROWS     1   /* the DMA_DATA rows of the engine path */
#define BC250_GDI_BATCH_DISPATCH 2   /* a compute dispatch */
#define BC250_GDI_BATCH_NO_SPACE 3   /* unreachable after Begin said yes; nothing was written */

/* The state of one call's translation. The caller sets nothing by hand: Begin fills it and the
 * other two functions own it. Out is the DMA buffer in dwords and Ib its GPU virtual address. */
typedef struct BC250_GDI_BATCH {
    unsigned int* Out;
    unsigned int Capacity;              /* dwords of Out */
    unsigned long long Ib;
    unsigned int Used;                  /* dwords written so far */
    unsigned long long CodeVa;          /* 0 until the code block is in this buffer */
    unsigned int SubRects;              /* what Begin was promised */
    /* What happened, for the caller's counters. */
    unsigned int Items, RowItems, Dispatches, Fallbacks, Dwords;
} BC250_GDI_BATCH;

/* The worst case a batch of SubRects sub-rectangles can cost, in dwords, including the cache
 * operation, the code block, one bounded dispatch for each sub-rectangle and the ring padding.
 * 0 when the number cannot be expressed in an unsigned int, which refuses the call. */
unsigned int Bc250GdiBatchDwords(unsigned int SubRects);

/* The most sub-rectangles a command buffer of CommandBytes bytes can hold, by the bound above. */
unsigned int Bc250GdiMaxSubRects(unsigned int CommandBytes);

/* The worst case any command buffer of CommandBytes bytes can cost, in dwords: the capacity a call
 * must have for Bc250GdiBatchBegin to be unable to refuse it. 0 when it does not fit an unsigned
 * int. */
unsigned int Bc250GdiCallDwords(unsigned int CommandBytes);

/* Rule 3: the dwords one call may write. DmaDwords is the space dxgkrnl reports for this call and
 * AheadDwords is what Bc250GdiDmaClaim established lies in the same DMA buffer before it, so that
 * several calls into one buffer cannot together exceed what the submitter can encode. 0 means this
 * call has no room at all and must translate nothing. */
unsigned int Bc250GdiCallCapacity(unsigned int DmaDwords, unsigned int AheadDwords);

/* The DMA buffer size to ask dxgkrnl for, in bytes, so that every command buffer up to
 * CommandBytesBound translates: Bc250GdiCallDwords rounded up to a page. 0 when that worst case
 * exceeds either ceiling, BC250_GDI_IB_DWORDS_MAX or BC250_GDI_DMA_BYTES_MAX. */
unsigned int Bc250GdiContextDmaBytes(unsigned int CommandBytesBound);

/* The command-buffer bound one GDI context is created with: the largest of BC250_GDI_CMDBUF_BOUND,
 * its halves down to BC250_GDI_CMDBUF_FLOOR and Wanted whose DMA buffer is at most
 * BC250_GDI_DMA_APERTURE_SHARE of ApertureBytes. Wanted is what an earlier call asked for past the
 * bound (GdiCmdBytesWanted) and 0 when nothing has; it is honoured only in the narrow range above
 * the bound that one indirect buffer still carries. An ApertureBytes of 0 means the aperture is not
 * known, and then the floor is used. The answer is never 0 and never above what
 * Bc250GdiContextDmaBytes can meet, so the caller always has a bound it can ask for. */
unsigned int Bc250GdiContextBound(unsigned long long ApertureBytes, unsigned int Wanted);

/* Begin a translation into Out. Capacity is what Bc250GdiCallCapacity answered; a larger number is
 * clamped to BC250_GDI_IB_DWORDS_MAX here, so no caller can hand the row path space the submitter
 * cannot encode. Returns 0 when this batch cannot be guaranteed, having written nothing: the caller
 * must then emit nothing at all and count the refusal. */
int Bc250GdiBatchBegin(BC250_GDI_BATCH* Batch, unsigned int* Out, unsigned int Capacity,
                       unsigned long long Ib, unsigned int SubRects);

/* One admitted sub-rectangle. Remaining is how many sub-rectangles the caller may still offer,
 * this one included, so that this item cannot spend the capacity the others are owed. Pattern is
 * the fill dword of a Bc250GdiWorkFill item, in the destination surface's own channel order.
 *
 * Returns a BC250_GDI_BATCH_* result. The cache operation is emitted before the first item, and
 * the code block before the first dispatch, so a batch whose items are all refused writes nothing.
 */
int Bc250GdiBatchItem(BC250_GDI_BATCH* Batch, const BC250_GDI_OP* Op,
                      const BC250_GDI_SURFACE* Surfaces, const unsigned long long* Vas,
                      unsigned int Count, const BC250_GDI_ITEM* Item, unsigned int Pattern,
                      unsigned int Remaining);

/* Pad the tail to the ring granule and return the dwords the caller must advance pDmaBuffer by. 0
 * when nothing was emitted. */
unsigned int Bc250GdiBatchEnd(BC250_GDI_BATCH* Batch);

#endif
