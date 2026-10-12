// Native DDC on the DDC1 pins. No DP AUX packets, no arbitrary I2C passthrough.
// The owner serializes this session with all AUX and display discovery work.
#pragma once
#include "../kmd/display_io.h"

typedef struct bc250_ddc_session {
    BC250_DISP_IO *io;
    unsigned long mask_before, aux_before, setup_before, speed_before;
    unsigned long transaction_before[4], control_before, arbitration_before;
    unsigned long open, saved, acquired, pad_changed, engine_changed;
    unsigned long transactions, timeouts, nacks, status;
    long first_failure, cleanup_failure;
} BC250_DDC_SESSION;

// All waits, including error cleanup, share Io->BudgetUs. Another engine owner
// is a refusal, never a request to reset that owner's transaction.
long bc250_ddc_open(BC250_DDC_SESSION *session, BC250_DISP_IO *io);
long bc250_ddc_close(BC250_DDC_SESSION *session);
long bc250_ddc_read_edid(BC250_DDC_SESSION *session, unsigned char *bytes,
                         unsigned long capacity, unsigned long *length);

enum bc250_adapter_type { BC250_ADAPTER_UNKNOWN, BC250_ADAPTER_TYPE1_HDMI,
    BC250_ADAPTER_TYPE2_HDMI, BC250_ADAPTER_TYPE2_OTHER };
typedef struct bc250_dual_mode_adapter {
    unsigned long type, max_tmds_khz;
    unsigned char bytes[32];
    long status;
} BC250_DUAL_MODE_ADAPTER;
long bc250_ddc_read_adapter(BC250_DDC_SESSION *session, BC250_DUAL_MODE_ADAPTER *adapter);
// Pure decoder for captured adapter bytes. Unknown is distinct from DVI: a
// missing response cannot establish the cable type or override the sink EDID.
void bc250_ddc_decode_adapter(const unsigned char *bytes, unsigned long length,
                              BC250_DUAL_MODE_ADAPTER *adapter);
