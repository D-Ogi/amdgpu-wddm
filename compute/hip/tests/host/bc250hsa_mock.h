/* bc250hsa_mock.h - the knobs of the mock device half of bc250hsa.
 *
 * Layer 2 of docs/design/m16-hip-route-b.md links tests/host/bc250hsa_mock.c in place of
 * bc250hsa/kmt_device.c, kmt_memory.c and submit.c. Its HIP test then runs the real
 * loader, the real argument packer and the real PM4 builder on a machine with no BC-250
 * adapter, and only the four display driver interface calls are replaced.
 *
 * The numbers bc250hsa_props_read() reports through the mock are mock values. They are
 * not measurements of the part, and nothing may cite them as such.
 */
#ifndef BC250HSA_MOCK_H
#define BC250HSA_MOCK_H

#include "bc250hsa.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bc250hsa_mock_config {
    uint32_t struct_bytes;
    uint32_t complete_submissions; /* 1 (the default): a submission retires at once.
                                    * 0: the fence never moves, so a caller's bounded
                                    * wait returns BC250HSA_ETIMEOUT */
    uint32_t lose_device_after;    /* 0: never. N: the Nth submission loses the device */
    uint32_t fail_alloc_after;     /* 0: never. N: the Nth allocation returns ENOMEM */
    uint32_t refuse_host_mapping;  /* 1: BC250HSA_MEM_DEVICE has no host mapping, which
                                    * is open question 1 of the design */
} bc250hsa_mock_config;

/* Applies the configuration. A NULL pointer restores the defaults. */
bc250hsa_status bc250hsa_mock_configure(const bc250hsa_mock_config* config);

/* How many submissions the mock has accepted, and the dwords of the last one. The second
 * is also what bc250hsa_last_ib() returns. */
uint64_t bc250hsa_mock_submission_count(void);

#ifdef __cplusplus
}
#endif

#endif /* BC250HSA_MOCK_H */
