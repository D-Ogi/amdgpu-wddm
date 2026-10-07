// DisplayPort AUX transactions through the DCN 2.0.1 software AUX engine (DP_AUX0, the DDC1 pads on unit A), and the
// EDID read over I2C-over-AUX. Plain C over display_io.h: modeset.c runs it at PASSIVE_LEVEL with BAR5 and a busy
// stall, driver\kmd\test\dpaux_test.c runs it against a model of the engine.
//
// The register sequence is Linux amdgpu's (drivers/gpu/drm/amd/display/dc/dce/dce_aux.c, MIT, v6.18):
// acquire_engine, submit_channel_request, get_channel_status, read_channel_reply and release_engine, and for the pad
// gpio/hw_ddc.c set_config (GPIO_DDC_CONFIG_TYPE_MODE_AUX). The E03 trace shows amdgpu run exactly this sequence on
// unit A (evidence/linux/2026-09-21-E03-init-trace/amdgpu-events.txt, 204 transactions). Every wait is a poll of
// SW_STATUS in 10 us stalls with a bound, and the whole session has a stall budget (BC250_DISP_IO.BudgetUs).
#pragma once
#include "display_io.h"

// The request header's action nibble (Linux enum i2caux_transaction_action).
#define BC250_AUX_I2C_WRITE 0x00ul
#define BC250_AUX_I2C_READ 0x10ul
#define BC250_AUX_I2C_WRITE_MOT 0x40ul
#define BC250_AUX_I2C_READ_MOT 0x50ul
#define BC250_AUX_DP_WRITE 0x80ul
#define BC250_AUX_DP_READ 0x90ul

// The reply code: the first reply byte shifted right by 4 (Linux read_channel_reply).
#define BC250_AUX_REPLY_ACK 0x0ul
#define BC250_AUX_REPLY_NACK 0x1ul
#define BC250_AUX_REPLY_DEFER 0x2ul
#define BC250_AUX_REPLY_I2C_NACK 0x4ul
#define BC250_AUX_REPLY_I2C_DEFER 0x8ul

#define BC250_AUX_MAX_PAYLOAD 16ul              // one AUX transaction carries at most 16 data bytes
#define BC250_AUX_POLL_US 10ul                  // Linux's REG_WAIT step for SW_DONE
#define BC250_AUX_WAIT_MAX_US 4000ul            // per wait; E03 measured 780-920 us per transaction
#define BC250_AUX_DEFER_US 1000ul               // the stall after a DEFER reply before the retry
// Linux dce_aux_transfer_with_retries: AUX_MIN_DEFER_RETRIES 7, AUX_MAX_I2C_DEFER_RETRIES 7,
// AUX_MAX_TIMEOUT_RETRIES 3, AUX_MAX_INVALID_REPLY_RETRIES 2.
#define BC250_AUX_DEFER_RETRIES 7ul
#define BC250_AUX_TIMEOUT_RETRIES 3ul
#define BC250_AUX_INVALID_RETRIES 2ul
#define BC250_AUX_EDID_ADDRESS 0x50ul           // the EDID's I2C address
#define BC250_AUX_SEGMENT_ADDRESS 0x30ul        // the E-DDC segment pointer

// The engine's verdict for one attempt (Linux enum aux_return_code_type, reduced).
enum bc250_aux_result {
    BC250_AUX_OK = 0,
    BC250_AUX_HPD_DISCON,
    BC250_AUX_TIMEOUT,
    BC250_AUX_INVALID_REPLY,
    BC250_AUX_ENGINE_BUSY,                  // the arbiter gave the engine to the DMCU, or did not grant it
    BC250_AUX_REGISTER,                     // a register access failed (table refusal or I/O error)
    BC250_AUX_RESULT_COUNT
};

typedef struct _BC250_AUX_SESSION {
    BC250_DISP_IO* Io;
    int Open;
    int PadModeSet;                         // AUX_PAD1_MODE was 0 and this session set it (Linux does the same)
    unsigned long DdcMaskBefore, AuxCtrl5Before, AuxControl;    // the pad and engine state found at open
    unsigned long Transactions, Attempts, Defers, Timeouts, Invalid, Nacks, Busy;
    unsigned long LastStatus, LastReply, LastResult;
} BC250_AUX_SESSION;

// Configure the pad for AUX (hw_ddc.c set_config) and note what was there. Fails with BC250_DISP_STATUS_MISMATCH
// when AUX_CONTROL.AUX_EN is 0: the firmware left the engine off, and this code does not run the enable-and-reset
// sequence of acquire_engine on an engine it did not see working (docs/design/display-modes.md, the AUX section).
long Bc250AuxOpen(BC250_AUX_SESSION* Session, BC250_DISP_IO* Io);
// Put DDC_PAD1_I2CMODE back to the value found at open, and AUX_PAD1_MODE when this session set it.
long Bc250AuxClose(BC250_AUX_SESSION* Session);

// One AUX request with Linux's retry policy. Action is a BC250_AUX_* action, Address 20 bits, Length 0..16. For a
// write, Data holds Length bytes. For a read, Data receives up to Length bytes and *Got says how many came back.
// Returns 0 on an ACK, BC250_DISP_STATUS_DEVICE on a NACK or after the retries, or the register/budget failure.
long Bc250AuxTransfer(BC250_AUX_SESSION* Session, unsigned long Action, unsigned long Address, unsigned char* Data,
                      unsigned long Length, unsigned long* Got);
// A native DPCD read of Length bytes from Address, in transactions of up to 16 bytes.
long Bc250AuxDpcdRead(BC250_AUX_SESSION* Session, unsigned long Address, unsigned char* Data, unsigned long Length);
// The monitor's EDID: block 0, then as many extensions as block 0 announces and Capacity holds, each block read as
// amdgpu reads it (segment pointer for blocks 2 and up, the offset write, 16-byte I2C reads, the stop). *Length is
// the number of bytes read, also after a failure (the blocks before the one that failed are whole). The checksums
// are not judged here (edid.c does that).
long Bc250AuxReadEdid(BC250_AUX_SESSION* Session, unsigned char* Buffer, unsigned long Capacity, unsigned long* Length);

const char* Bc250AuxResultText(unsigned long Result);
