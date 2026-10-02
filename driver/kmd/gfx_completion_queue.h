/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GFX_COMPLETION_QUEUE_H
#define BC250_GFX_COMPLETION_QUEUE_H
/* Bounded independently of CP fetch progress: consumed ring words do not imply
 * completed shaders or permission to retire the scheduler fence. Caller locks. */
#define BC250_GFX_PENDING_MAX 7u
typedef struct BC250_GFX_COMPLETION {
    unsigned int Seq, Fence, ReportFence, Node;
    unsigned long long Epoch, Deadline;
    /* KMD193 (bsod-245): the queue entry of the faulting job named a sequence, a fence and a node and nothing
     * else, so the dump could not say which context - let alone which process - had submitted it. Values only,
     * never dereferenced: the context object may be freed before anyone reads them. */
    unsigned long long Context;     /* the submitting BC250_WDDM_OBJECT as a value */
    unsigned long ProcessId;        /* the process that created that context */
    unsigned long ContextFlags;     /* BC250_PJ_CTX_* (bc250kmd_escape.h) */
} BC250_GFX_COMPLETION;
typedef struct BC250_GFX_COMPLETION_QUEUE {
    BC250_GFX_COMPLETION Items[BC250_GFX_PENDING_MAX];
    unsigned int Head, Count;
} BC250_GFX_COMPLETION_QUEUE;
static __inline BC250_GFX_COMPLETION* Bc250GfxQueueHead(BC250_GFX_COMPLETION_QUEUE* q)
{
    return q->Count ? &q->Items[q->Head] : 0;
}
static __inline BC250_GFX_COMPLETION* Bc250GfxQueueTail(BC250_GFX_COMPLETION_QUEUE* q)
{
    return q->Count ? &q->Items[(q->Head + q->Count - 1u) % BC250_GFX_PENDING_MAX] : 0;
}
static __inline int Bc250GfxQueuePush(BC250_GFX_COMPLETION_QUEUE* q, BC250_GFX_COMPLETION job)
{
    if (q->Count == BC250_GFX_PENDING_MAX) return 0;
    q->Items[(q->Head + q->Count) % BC250_GFX_PENDING_MAX] = job;
    ++q->Count;
    return 1;
}
static __inline void Bc250GfxQueuePop(BC250_GFX_COMPLETION_QUEUE* q)
{
    if (q->Count) { q->Head = (q->Head + 1u) % BC250_GFX_PENDING_MAX; --q->Count; }
}
#endif
