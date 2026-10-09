// DisplayPort AUX and the EDID read: see dpaux.h.
//
// PROVENANCE: Linux AMD display code (MIT), v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449: dc/dce/dce_aux.c
// acquire_engine, release_engine, submit_channel_request, get_channel_status, read_channel_reply and the retry
// limits of dce_aux_transfer_with_retries; dc/gpio/hw_ddc.c set_config. Our own code over those sequences, with
// every register named from dcn_2_0_1_offset.h / dcn_2_0_1_sh_mask.h through gen_regs.py.
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dpaux.h"
#include "edid.h"

#define R_CONTROL BC250_REG_DMU_DP_AUX0_AUX_CONTROL
#define R_SW_CONTROL BC250_REG_DMU_DP_AUX0_AUX_SW_CONTROL
#define R_ARB BC250_REG_DMU_DP_AUX0_AUX_ARB_CONTROL
#define R_INT BC250_REG_DMU_DP_AUX0_AUX_INTERRUPT_CONTROL
#define R_STATUS BC250_REG_DMU_DP_AUX0_AUX_SW_STATUS
#define R_DATA BC250_REG_DMU_DP_AUX0_AUX_SW_DATA
#define R_DDC_MASK BC250_REG_DMU_DC_GPIO_DDC1_MASK
#define R_AUX_CTRL5 BC250_REG_DMU_DC_GPIO_AUX_CTRL_5

#define SW_CAN_ACCESS_AUX 1ul                  // dce_aux.c: AUX_REG_RW_CNTL_STATUS values
#define DMCU_CAN_ACCESS_AUX 2ul

#define TRY(x) do { long s_ = (x); if (s_ < 0) return s_; } while (0)

const char* Bc250AuxResultText(unsigned long Result)
{
    switch (Result) {
    case BC250_AUX_OK: return "ok";
    case BC250_AUX_HPD_DISCON: return "HPD low";
    case BC250_AUX_TIMEOUT: return "timeout";
    case BC250_AUX_INVALID_REPLY: return "invalid reply";
    case BC250_AUX_ENGINE_BUSY: return "engine not granted";
    case BC250_AUX_REGISTER: return "register access failed";
    default: return "?";
    }
}

long Bc250AuxOpen(BC250_AUX_SESSION* Session, BC250_DISP_IO* Io)
{
    unsigned long v;

    Session->Io = Io;
    Session->Open = 0;
    Session->PadModeSet = 0;
    Session->Transactions = Session->Attempts = Session->Defers = Session->Timeouts = 0;
    Session->Invalid = Session->Nacks = Session->Busy = 0;
    Session->LastStatus = Session->LastReply = Session->LastResult = 0;
    TRY(Bc250DispRead(Io, R_CONTROL, &Session->AuxControl));
    TRY(Bc250DispRead(Io, R_DDC_MASK, &Session->DdcMaskBefore));
    TRY(Bc250DispRead(Io, R_AUX_CTRL5, &Session->AuxCtrl5Before));
    // Deviation from acquire_engine: an engine the firmware left disabled is not enabled and reset here. The GOP
    // reads the EDID itself, so an enabled engine is the expected state; anything else is a refusal to log.
    if ((Session->AuxControl & DP_AUX0_AUX_CONTROL__AUX_EN_MASK) == 0) return BC250_DISP_STATUS_MISMATCH;
    // hw_ddc.c set_config, GPIO_DDC_CONFIG_TYPE_MODE_AUX: AUX_PAD1_MODE 1 if it is 0, then DDC_PAD_I2CMODE 0.
    if ((Session->DdcMaskBefore & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK) == 0) {
        TRY(Bc250DispWrite(Io, R_DDC_MASK, Session->DdcMaskBefore | DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK));
        Session->PadModeSet = 1;
    }
    TRY(Bc250DispRead(Io, R_AUX_CTRL5, &v));
    TRY(Bc250DispWrite(Io, R_AUX_CTRL5, v & ~DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK));
    Session->Open = 1;
    return BC250_DISP_STATUS_SUCCESS;
}

// Not in Linux, which leaves the pad in AUX mode: the next owner (Basic Display, the firmware after a restart) finds
// the two named bits as the firmware left them. Every other bit of both registers is written back as read.
long Bc250AuxClose(BC250_AUX_SESSION* Session)
{
    BC250_DISP_IO* io = Session->Io;
    long first = BC250_DISP_STATUS_SUCCESS, s;

    if (!Session->Open) return BC250_DISP_STATUS_SUCCESS;
    Session->Open = 0;
    s = Bc250DispUpdate(io, R_AUX_CTRL5, DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK, Session->AuxCtrl5Before);
    if (s < 0) first = s;
    if (Session->PadModeSet) {
        s = Bc250DispUpdate(io, R_DDC_MASK, DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK, 0);
        if (s < 0 && first >= 0) first = s;
    }
    return first;
}

// acquire_engine without the enable branch (Bc250AuxOpen checked AUX_EN): refuse when the DMCU holds the engine,
// request it for software, and require the grant.
static unsigned long Acquire(BC250_DISP_IO* Io)
{
    unsigned long v;
    if (Bc250DispRead(Io, R_ARB, &v) < 0) return BC250_AUX_REGISTER;
    if (Bc250DispGet(v, DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK) == DMCU_CAN_ACCESS_AUX)
        return BC250_AUX_ENGINE_BUSY;
    if (Bc250DispUpdate(Io, R_ARB, DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK,
                        DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK) < 0) return BC250_AUX_REGISTER;
    if (Bc250DispRead(Io, R_ARB, &v) < 0) return BC250_AUX_REGISTER;
    return Bc250DispGet(v, DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK) == SW_CAN_ACCESS_AUX
        ? BC250_AUX_OK : BC250_AUX_ENGINE_BUSY;
}

// release_engine: AUX_SW_DONE_USING_AUX_REG 1, AUX_SW_USE_AUX_REG_REQ 0.
static void Release(BC250_DISP_IO* Io)
{
    (void)Bc250DispUpdate(Io, R_ARB,
                          DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK |
                          DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK,
                          DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK);
}

// REG_WAIT(AUX_SW_STATUS, AUX_SW_DONE, Want, 10, ...): poll in BC250_AUX_POLL_US stalls, at most
// BC250_AUX_WAIT_MAX_US. Returns the last status read, or a failure.
static long WaitDone(BC250_DISP_IO* Io, unsigned long Want, unsigned long* Status)
{
    unsigned long waited = 0;
    for (;;) {
        TRY(Bc250DispRead(Io, R_STATUS, Status));
        if (((*Status & DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK) != 0) == (Want != 0)) return BC250_DISP_STATUS_SUCCESS;
        if (waited >= BC250_AUX_WAIT_MAX_US) return BC250_DISP_STATUS_TIMEOUT;
        TRY(Bc250DispStall(Io, BC250_AUX_POLL_US));
        waited += BC250_AUX_POLL_US;
    }
}

// One attempt: submit_channel_request, get_channel_status and read_channel_reply. *Result is the engine's verdict;
// *Reply the reply code when the engine returned one; Data and *Got the reply's data bytes for an ACK.
static long Attempt(BC250_AUX_SESSION* Session, unsigned long Action, unsigned long Address, unsigned char* Data,
                    unsigned long Length, int IsWrite, unsigned long* Result, unsigned long* Reply, unsigned long* Got)
{
    BC250_DISP_IO* io = Session->Io;
    unsigned long v, status, bytes, i, wr;
    long s;

    *Result = BC250_AUX_REGISTER;
    *Reply = 0;
    *Got = 0;
    Session->Attempts++;
    // AUX_SW_DONE_ACK, then wait for SW_DONE to drop.
    TRY(Bc250DispUpdate(io, R_INT, DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK,
                        DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK));
    s = WaitDone(io, 0, &status);
    if (s == BC250_DISP_STATUS_TIMEOUT) { *Result = BC250_AUX_TIMEOUT; return BC250_DISP_STATUS_SUCCESS; }
    TRY(s);
    // The header (action and the 20-bit address) is 3 bytes, a length byte follows when Length is not 0, and a
    // write carries its payload. START_DELAY 0, as amdgpu's EDID and DPCD requests on unit A.
    wr = (Length ? 4ul : 3ul) + (IsWrite ? Length : 0ul);
    TRY(Bc250DispUpdate(io, R_SW_CONTROL,
                        DP_AUX0_AUX_SW_CONTROL__AUX_SW_START_DELAY_MASK | DP_AUX0_AUX_SW_CONTROL__AUX_SW_WR_BYTES_MASK,
                        Bc250DispField(DP_AUX0_AUX_SW_CONTROL__AUX_SW_WR_BYTES_MASK, wr)));
    // REG_UPDATE_4(AUX_SW_DATA, INDEX 0, DATA_RW 0, AUTOINCREMENT_DISABLE 1, DATA header) sets the index; the
    // REG_SETs after it reuse that value with AUTOINCREMENT_DISABLE 0.
    TRY(Bc250DispRead(io, R_DATA, &v));
    v &= ~(DP_AUX0_AUX_SW_DATA__AUX_SW_INDEX_MASK | DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_RW_MASK |
           DP_AUX0_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK | DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK);
    TRY(Bc250DispWrite(io, R_DATA, v | DP_AUX0_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK |
                       Bc250DispField(DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK, Action | ((Address & 0xF0000ul) >> 16))));
    TRY(Bc250DispWrite(io, R_DATA, v | Bc250DispField(DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK, (Address & 0xFF00ul) >> 8)));
    TRY(Bc250DispWrite(io, R_DATA, v | Bc250DispField(DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK, Address & 0xFFul)));
    if (Length) TRY(Bc250DispWrite(io, R_DATA, v | Bc250DispField(DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK, Length - 1)));
    if (IsWrite)
        for (i = 0; i < Length; i++)
            TRY(Bc250DispWrite(io, R_DATA, v | Bc250DispField(DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK, Data[i])));
    TRY(Bc250DispUpdate(io, R_SW_CONTROL, DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK, DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK));

    // get_channel_status.
    s = WaitDone(io, 1, &status);
    Session->LastStatus = status;
    if (s == BC250_DISP_STATUS_TIMEOUT) { *Result = BC250_AUX_TIMEOUT; return BC250_DISP_STATUS_SUCCESS; }
    TRY(s);
    if (status & DP_AUX0_AUX_SW_STATUS__AUX_SW_HPD_DISCON_MASK) { *Result = BC250_AUX_HPD_DISCON; return BC250_DISP_STATUS_SUCCESS; }
    if (status & (DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_STATE_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_MASK)) {
        *Result = BC250_AUX_TIMEOUT;
        return BC250_DISP_STATUS_SUCCESS;
    }
    if (status & (DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_INVALID_STOP_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_NO_DET_MASK |
                  DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_H_MASK |
                  DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_L_MASK)) {
        *Result = BC250_AUX_INVALID_REPLY;
        return BC250_DISP_STATUS_SUCCESS;
    }
    bytes = Bc250DispGet(status, DP_AUX0_AUX_SW_STATUS__AUX_SW_REPLY_BYTE_COUNT_MASK);
    if (bytes == 0) { *Result = BC250_AUX_INVALID_REPLY; return BC250_DISP_STATUS_SUCCESS; }

    // read_channel_reply: REG_UPDATE_SEQ_3(AUX_SW_DATA, INDEX 0, AUTOINCREMENT_DISABLE 1, DATA_RW 1) is one read and
    // three writes, one field each (E03: 0x00000100, 0x80000100, 0x80000101 after a read of 0x00050100).
    TRY(Bc250DispRead(io, R_DATA, &v));
    v &= ~DP_AUX0_AUX_SW_DATA__AUX_SW_INDEX_MASK;
    TRY(Bc250DispWrite(io, R_DATA, v));
    v |= DP_AUX0_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK;
    TRY(Bc250DispWrite(io, R_DATA, v));
    v |= DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_RW_MASK;
    TRY(Bc250DispWrite(io, R_DATA, v));
    TRY(Bc250DispRead(io, R_DATA, &v));
    *Reply = Bc250DispGet(v, DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK) >> 4;
    *Result = BC250_AUX_OK;
    if (*Reply == BC250_AUX_REPLY_ACK) {
        bytes--;                                // the first byte was the reply code
        if (bytes > Length) { *Result = BC250_AUX_INVALID_REPLY; return BC250_DISP_STATUS_SUCCESS; }
        for (i = 0; i < bytes; i++) {
            TRY(Bc250DispRead(io, R_DATA, &v));
            if (!IsWrite) Data[i] = (unsigned char)Bc250DispGet(v, DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK);
        }
        *Got = bytes;
    }
    return BC250_DISP_STATUS_SUCCESS;
}

long Bc250AuxTransfer(BC250_AUX_SESSION* Session, unsigned long Action, unsigned long Address, unsigned char* Data,
                      unsigned long Length, unsigned long* Got)
{
    BC250_DISP_IO* io = Session->Io;
    const int isWrite = Action == BC250_AUX_DP_WRITE || Action == BC250_AUX_I2C_WRITE || Action == BC250_AUX_I2C_WRITE_MOT;
    unsigned long defers = 0, timeouts = 0, invalid = 0, result, reply, got;
    long s;

    *Got = 0;
    if (!Session->Open || Length > BC250_AUX_MAX_PAYLOAD || Address > 0xFFFFFul || (Length && Data == 0))
        return BC250_DISP_STATUS_INVALID;
    Session->Transactions++;
    for (;;) {
        result = Acquire(io);
        if (result != BC250_AUX_OK) {
            Session->LastResult = result;
            if (result == BC250_AUX_ENGINE_BUSY) { Session->Busy++; Release(io); return BC250_DISP_STATUS_BUSY; }
            return BC250_DISP_STATUS_DEVICE;
        }
        s = Attempt(Session, Action, Address, Data, Length, isWrite, &result, &reply, &got);
        Release(io);
        Session->LastResult = result;
        Session->LastReply = reply;
        if (s < 0) return s;
        switch (result) {
        case BC250_AUX_OK:
            if (reply == BC250_AUX_REPLY_ACK) { *Got = got; return BC250_DISP_STATUS_SUCCESS; }
            if (reply == BC250_AUX_REPLY_DEFER || reply == BC250_AUX_REPLY_I2C_DEFER) {
                Session->Defers++;
                if (++defers > BC250_AUX_DEFER_RETRIES) return BC250_DISP_STATUS_DEVICE;
                TRY(Bc250DispStall(io, BC250_AUX_DEFER_US));
                continue;
            }
            Session->Nacks++;                   // NACK, I2C NACK or a reply code nobody defines
            return BC250_DISP_STATUS_DEVICE;
        case BC250_AUX_TIMEOUT:
            Session->Timeouts++;
            if (++timeouts > BC250_AUX_TIMEOUT_RETRIES) return BC250_DISP_STATUS_TIMEOUT;
            continue;
        case BC250_AUX_INVALID_REPLY:
            Session->Invalid++;
            if (++invalid > BC250_AUX_INVALID_RETRIES) return BC250_DISP_STATUS_DEVICE;
            continue;
        default:                                // HPD low: the sink is gone, no retry
            return BC250_DISP_STATUS_DEVICE;
        }
    }
}

long Bc250AuxDpcdRead(BC250_AUX_SESSION* Session, unsigned long Address, unsigned char* Data, unsigned long Length)
{
    unsigned long done = 0, got;
    while (done < Length) {
        unsigned long n = Length - done > BC250_AUX_MAX_PAYLOAD ? BC250_AUX_MAX_PAYLOAD : Length - done;
        TRY(Bc250AuxTransfer(Session, BC250_AUX_DP_READ, Address + done, Data + done, n, &got));
        if (got == 0) return BC250_DISP_STATUS_DEVICE;
        done += got;                            // a short reply is followed by a request for the rest
    }
    return BC250_DISP_STATUS_SUCCESS;
}

// One 128-byte block as amdgpu reads it on unit A (E03): the segment pointer when the block is past the first
// segment (not in the trace: unit A's EDID has two blocks), address-only write, the offset write, the read
// restart, 16-byte reads with MOT, and the address-only read without MOT, which is the stop.
static long ReadBlock(BC250_AUX_SESSION* Session, unsigned long Block, unsigned char* Out)
{
    unsigned char offset = (unsigned char)((Block & 1ul) * BC250_EDID_BLOCK);
    unsigned char segment = (unsigned char)(Block / 2ul);
    unsigned long done = 0, got;
    long s;

    if (segment != 0)
        TRY(Bc250AuxTransfer(Session, BC250_AUX_I2C_WRITE_MOT, BC250_AUX_SEGMENT_ADDRESS, &segment, 1, &got));
    TRY(Bc250AuxTransfer(Session, BC250_AUX_I2C_WRITE_MOT, BC250_AUX_EDID_ADDRESS, 0, 0, &got));
    TRY(Bc250AuxTransfer(Session, BC250_AUX_I2C_WRITE_MOT, BC250_AUX_EDID_ADDRESS, &offset, 1, &got));
    TRY(Bc250AuxTransfer(Session, BC250_AUX_I2C_READ_MOT, BC250_AUX_EDID_ADDRESS, 0, 0, &got));
    s = BC250_DISP_STATUS_SUCCESS;
    while (done < BC250_EDID_BLOCK) {
        unsigned long n = BC250_EDID_BLOCK - done > BC250_AUX_MAX_PAYLOAD ? BC250_AUX_MAX_PAYLOAD
                                                                                 : BC250_EDID_BLOCK - done;
        s = Bc250AuxTransfer(Session, BC250_AUX_I2C_READ_MOT, BC250_AUX_EDID_ADDRESS, Out + done, n, &got);
        if (s < 0) break;
        if (got == 0) { s = BC250_DISP_STATUS_DEVICE; break; }
        done += got;                            // an I2C read may come back short: the next request continues
    }
    // The stop, also after a failed read, so the sink's I2C state machine is not left mid-transfer.
    {
        long stop = Bc250AuxTransfer(Session, BC250_AUX_I2C_READ, BC250_AUX_EDID_ADDRESS, 0, 0, &got);
        if (s >= 0) s = stop;
    }
    return s;
}

long Bc250AuxReadEdid(BC250_AUX_SESSION* Session, unsigned char* Buffer, unsigned long Capacity, unsigned long* Length)
{
    unsigned long blocks, b;

    *Length = 0;
    if (Capacity < BC250_EDID_BLOCK) return BC250_DISP_STATUS_INVALID;
    TRY(ReadBlock(Session, 0, Buffer));
    *Length = BC250_EDID_BLOCK;
    blocks = 1ul + Buffer[126];
    if (blocks > Capacity / BC250_EDID_BLOCK) blocks = Capacity / BC250_EDID_BLOCK;
    for (b = 1; b < blocks; b++) {
        TRY(ReadBlock(Session, b, Buffer + b * BC250_EDID_BLOCK));
        *Length += BC250_EDID_BLOCK;
    }
    return BC250_DISP_STATUS_SUCCESS;
}
