/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GFX_COMPLETION_QUEUE_H
#define BC250_GFX_COMPLETION_QUEUE_H
/* Bounded independently of CP fetch progress: consumed ring words do not imply
 * completed shaders or permission to retire the scheduler fence. Caller locks. */
#define BC250_GFX_PENDING_MAX 7u
typedef struct BC250_GFX_COMPLETION {
    unsigned int Seq, Fence, ReportFence, Node;
    unsigned long long Epoch, Deadline;
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
