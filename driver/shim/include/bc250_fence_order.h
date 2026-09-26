/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_FENCE_ORDER_H
#define BC250_FENCE_ORDER_H
static __inline int bc250_fence_reached(unsigned int observed, unsigned int requested)
{
    return observed != 0 && requested != 0 && (observed - requested) < 0x80000000u;
}
#endif
