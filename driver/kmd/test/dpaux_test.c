// Host test of dpaux.c and display_io.c: the DP AUX engine sequence and the EDID read over I2C-over-AUX, driven
// against a model of the DCN 2.0.1 software AUX engine (DP_AUX0) and of a DisplayPort sink with an E-DDC EDID
// at I2C 0x50 (segment pointer 0x30) and a DPCD. Built with no WDK header by run_modeset.ps1.
//
// The model follows the register behaviour the E03 trace shows on unit A (the arbiter grant in
// AUX_REG_RW_CNTL_STATUS, SW_DONE going to 0 after SW_DONE_ACK and to 1 after GO, REPLY_BYTE_COUNT, the reply code
// in the first data byte), and checks the request the code builds against what the sink would see. Positive
// controls: the lab EDID read back byte for byte with full and with short I2C replies, a 4-block EDID through the
// segment pointer, a DPCD read. Negative controls: DEFER and I2C DEFER up to and past the retry limit, NACK, I2C
// NACK, HPD low, reply timeout, a reply that never comes, invalid replies, a zero byte count, the DMCU owning the
// engine, an engine the firmware left off, a stall budget that runs out, and a register outside the table.
#include <stdio.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../dpaux.h"
#include "../edid.h"
#include "edid_lab_redacted.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// ---- the model ---------------------------------------------------------------------------------------------------

enum fault { F_NONE, F_DEFER, F_I2C_DEFER, F_NACK, F_I2C_NACK, F_HPD, F_RX_TIMEOUT, F_NEVER_DONE, F_INVALID, F_ZERO_BYTES };

typedef struct {
    // registers
    unsigned long auxControl, arb, intCtl, swControl, swStatus, ddcMask, auxCtrl5;
    unsigned long lastDataWrite;
    int dmcuOwns, granted;
    // the data buffer
    unsigned char tx[32], rx[32];
    unsigned long txIdx, rxIdx, rxLen;
    int readMode;
    // the transaction in flight
    int busy;
    unsigned long doneAtUs;
    unsigned long nowUs;
    // the sink
    unsigned char edid[512];
    unsigned long edidLen, i2cOffset, segment, shortReply;
    unsigned char dpcd[0x300];
    // fault script: the next FaultCount attempts get Fault
    enum fault fault;
    unsigned long faultCount;
    // observations
    unsigned long goes, acquires, releases, outsideTable, i2cStops, segmentWrites;
    unsigned long readsBeforeRelease;   // transactions started while the engine was not granted
    unsigned long writeFail;            // fail the write to this offset (0 = none)
} MODEL;

static MODEL g_m;

static void ModelReset(void)
{
    memset(&g_m, 0, sizeof(g_m));
    g_m.auxControl = DP_AUX0_AUX_CONTROL__AUX_EN_MASK;
    g_m.ddcMask = 0;                                    // AUX_PAD1_MODE 0: the open sets it
    g_m.auxCtrl5 = 0x0003F55Aul;                        // the firmware value E03 read (DDC_PAD1_I2CMODE 1)
    g_m.intCtl = 0x40;
    memcpy(g_m.edid, g_LabEdid, 256);
    g_m.edidLen = 256;
}

static unsigned long Status(unsigned long Bytes, unsigned long Extra)
{
    return DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK | Extra |
           ((Bytes << 24) & DP_AUX0_AUX_SW_STATUS__AUX_SW_REPLY_BYTE_COUNT_MASK);
}

// The sink's answer to the request in tx[].
static void Execute(void)
{
    unsigned long wrBytes = (g_m.swControl & DP_AUX0_AUX_SW_CONTROL__AUX_SW_WR_BYTES_MASK) >>
                            DP_AUX0_AUX_SW_CONTROL__AUX_SW_WR_BYTES__SHIFT;
    unsigned long action = g_m.tx[0] & 0xF0u, addr = ((g_m.tx[0] & 0x0Fu) << 16) | (g_m.tx[1] << 8) | g_m.tx[2];
    unsigned long len = wrBytes >= 4 ? g_m.tx[3] + 1ul : 0ul, i, n;
    int isWrite = action == BC250_AUX_DP_WRITE || action == BC250_AUX_I2C_WRITE || action == BC250_AUX_I2C_WRITE_MOT;
    int isI2c = (action & 0x80u) == 0;
    enum fault f = F_NONE;

    g_m.goes++;
    if (!g_m.granted) g_m.readsBeforeRelease++;
    g_m.busy = 1;
    g_m.doneAtUs = g_m.nowUs + 800;                     // E03: 780-920 us per transaction
    g_m.rxIdx = 0;
    g_m.readMode = 0;
    if (g_m.faultCount) { f = g_m.fault; g_m.faultCount--; }
    // The request must be complete: header, length byte when there is a length, and the payload of a write.
    if (wrBytes != (len ? 4ul : 3ul) + (isWrite ? len : 0ul) || g_m.txIdx != wrBytes) f = F_INVALID;
    switch (f) {
    case F_NEVER_DONE: g_m.doneAtUs = 0xFFFFFFFFul; g_m.swStatus = 0; return;
    case F_HPD: g_m.swStatus = Status(0, DP_AUX0_AUX_SW_STATUS__AUX_SW_HPD_DISCON_MASK); return;
    case F_RX_TIMEOUT: g_m.swStatus = Status(0, DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_MASK); return;
    case F_INVALID: g_m.swStatus = Status(1, DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_H_MASK); return;
    case F_ZERO_BYTES: g_m.swStatus = Status(0, 0); return;
    case F_DEFER: g_m.rx[0] = BC250_AUX_REPLY_DEFER << 4; g_m.rxLen = 1; g_m.swStatus = Status(1, 0); return;
    case F_I2C_DEFER: g_m.rx[0] = BC250_AUX_REPLY_I2C_DEFER << 4; g_m.rxLen = 1; g_m.swStatus = Status(1, 0); return;
    case F_NACK: g_m.rx[0] = BC250_AUX_REPLY_NACK << 4; g_m.rxLen = 1; g_m.swStatus = Status(1, 0); return;
    case F_I2C_NACK: g_m.rx[0] = BC250_AUX_REPLY_I2C_NACK << 4; g_m.rxLen = 1; g_m.swStatus = Status(1, 0); return;
    default: break;
    }
    g_m.rx[0] = BC250_AUX_REPLY_ACK << 4;
    g_m.rxLen = 1;
    if (isI2c) {
        if (addr == BC250_AUX_SEGMENT_ADDRESS && isWrite && len == 1) { g_m.segment = g_m.tx[4]; g_m.segmentWrites++; }
        else if (addr == BC250_AUX_EDID_ADDRESS && isWrite && len == 1) g_m.i2cOffset = g_m.tx[4];
        else if (addr == BC250_AUX_EDID_ADDRESS && !isWrite) {
            n = len;
            if (g_m.shortReply && n > g_m.shortReply) n = g_m.shortReply;
            for (i = 0; i < n; i++) {
                unsigned long at = g_m.segment * 256ul + g_m.i2cOffset;
                g_m.rx[1 + i] = at < g_m.edidLen ? g_m.edid[at] : 0xFF;
                g_m.i2cOffset = (g_m.i2cOffset + 1ul) & 0xFFul;
            }
            g_m.rxLen += n;
        } else if (addr != BC250_AUX_EDID_ADDRESS) {
            g_m.rx[0] = BC250_AUX_REPLY_I2C_NACK << 4;      // nobody at that address
        }
        if ((action & 0x40u) == 0) { g_m.segment = 0; g_m.i2cStops++; }    // a stop resets the segment pointer
    } else if (!isWrite) {
        for (i = 0; i < len; i++) g_m.rx[1 + i] = g_m.dpcd[(addr + i) % sizeof(g_m.dpcd)];
        g_m.rxLen += len;
    }
    g_m.swStatus = Status(g_m.rxLen, 0);
}

static long ModelRead(void* Context, unsigned long Offset, unsigned long* Value)
{
    (void)Context;
    switch (Offset) {
    case BC250_REG_DMU_DP_AUX0_AUX_CONTROL: *Value = g_m.auxControl; break;
    case BC250_REG_DMU_DP_AUX0_AUX_ARB_CONTROL:
        *Value = (g_m.arb & ~DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK) |
                 ((g_m.dmcuOwns ? 2ul : g_m.granted ? 1ul : 0ul) << DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS__SHIFT);
        break;
    case BC250_REG_DMU_DP_AUX0_AUX_INTERRUPT_CONTROL: *Value = g_m.intCtl; break;
    case BC250_REG_DMU_DP_AUX0_AUX_SW_CONTROL: *Value = g_m.swControl; break;
    case BC250_REG_DMU_DP_AUX0_AUX_SW_STATUS:
        if (g_m.busy && g_m.nowUs < g_m.doneAtUs) *Value = 0x61000000ul;          // E03: busy status, DONE 0
        else *Value = g_m.swStatus;
        break;
    case BC250_REG_DMU_DP_AUX0_AUX_SW_DATA:
        if (g_m.readMode) {
            *Value = (g_m.rxIdx < g_m.rxLen ? (unsigned long)g_m.rx[g_m.rxIdx] : 0ul) << 8;
            g_m.rxIdx++;
        } else *Value = g_m.lastDataWrite;
        break;
    case BC250_REG_DMU_DC_GPIO_DDC1_MASK: *Value = g_m.ddcMask; break;
    case BC250_REG_DMU_DC_GPIO_AUX_CTRL_5: *Value = g_m.auxCtrl5; break;
    default: g_m.outsideTable++; *Value = 0; break;
    }
    return 0;
}

static long ModelWrite(void* Context, unsigned long Offset, unsigned long Value)
{
    (void)Context;
    if (g_m.writeFail && Offset == g_m.writeFail) return (long)0xC0000185L;
    switch (Offset) {
    case BC250_REG_DMU_DP_AUX0_AUX_CONTROL: g_m.auxControl = Value; break;
    case BC250_REG_DMU_DP_AUX0_AUX_ARB_CONTROL:
        g_m.arb = Value & ~DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK;
        if ((Value & DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK) && !g_m.dmcuOwns && !g_m.granted) {
            g_m.granted = 1; g_m.acquires++;
        }
        if (Value & DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK) {
            if (g_m.granted) g_m.releases++;
            g_m.granted = 0;
            g_m.arb &= ~(DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK |
                         DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK);
        }
        break;
    case BC250_REG_DMU_DP_AUX0_AUX_INTERRUPT_CONTROL:
        g_m.intCtl = Value & ~DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK;
        if (Value & DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK) { g_m.swStatus = 0; g_m.busy = 0; g_m.txIdx = 0; }
        break;
    case BC250_REG_DMU_DP_AUX0_AUX_SW_CONTROL:
        g_m.swControl = Value & ~DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK;
        if (Value & DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK) Execute();
        break;
    case BC250_REG_DMU_DP_AUX0_AUX_SW_DATA:
        g_m.lastDataWrite = Value;
        if (Value & DP_AUX0_AUX_SW_DATA__AUX_SW_AUTOINCREMENT_DISABLE_MASK) {
            g_m.txIdx = (Value & DP_AUX0_AUX_SW_DATA__AUX_SW_INDEX_MASK) >> DP_AUX0_AUX_SW_DATA__AUX_SW_INDEX__SHIFT;
            g_m.rxIdx = g_m.txIdx;
        }
        if (Value & DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_RW_MASK) { g_m.readMode = 1; break; }
        if (!g_m.busy && g_m.txIdx < sizeof(g_m.tx))
            g_m.tx[g_m.txIdx++] = (unsigned char)((Value & DP_AUX0_AUX_SW_DATA__AUX_SW_DATA_MASK) >> 8);
        break;
    case BC250_REG_DMU_DC_GPIO_DDC1_MASK: g_m.ddcMask = Value; break;
    case BC250_REG_DMU_DC_GPIO_AUX_CTRL_5: g_m.auxCtrl5 = Value; break;
    default: g_m.outsideTable++; break;
    }
    return 0;
}

static void ModelStall(void* Context, unsigned long Us)
{
    (void)Context;
    g_m.nowUs += Us;
}

static void IoInit(BC250_DISP_IO* Io, unsigned long BudgetUs)
{
    memset(Io, 0, sizeof(*Io));
    Io->Read = ModelRead;
    Io->Write = ModelWrite;
    Io->Stall = ModelStall;
    Io->BudgetUs = BudgetUs;
}

// ---- tests -------------------------------------------------------------------------------------------------------

static void TestOpenClose(void)
{
    BC250_DISP_IO io;
    BC250_AUX_SESSION s;

    ModelReset();
    IoInit(&io, 1000000);
    CHECK(Bc250AuxOpen(&s, &io) == BC250_DISP_STATUS_SUCCESS);
    CHECK(s.PadModeSet == 1 && (g_m.ddcMask & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK));
    CHECK((g_m.auxCtrl5 & DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK) == 0);
    CHECK(g_m.auxCtrl5 == 0x0003E55Aul);                 // amdgpu's value in E03
    CHECK(Bc250AuxClose(&s) == BC250_DISP_STATUS_SUCCESS);
    CHECK(g_m.auxCtrl5 == 0x0003F55Aul && g_m.ddcMask == 0);
    CHECK(Bc250AuxClose(&s) == BC250_DISP_STATUS_SUCCESS);      // a second close does nothing

    // The pad already in AUX mode: the open leaves it and the close does not clear it.
    ModelReset();
    g_m.ddcMask = DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK;
    IoInit(&io, 1000000);
    CHECK(Bc250AuxOpen(&s, &io) == 0 && s.PadModeSet == 0);
    CHECK(Bc250AuxClose(&s) == 0 && g_m.ddcMask == DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK);

    // Negative: the engine the firmware left off is refused, nothing written.
    ModelReset();
    g_m.auxControl = 0;
    IoInit(&io, 1000000);
    CHECK(Bc250AuxOpen(&s, &io) == BC250_DISP_STATUS_MISMATCH);
    CHECK(io.Writes == 0 && g_m.auxCtrl5 == 0x0003F55Aul && !s.Open);
    {
        unsigned char b[1];
        unsigned long got;
        CHECK(Bc250AuxTransfer(&s, BC250_AUX_DP_READ, 0, b, 1, &got) == BC250_DISP_STATUS_INVALID);
    }
}

static void TestEdidRead(unsigned long ShortReply)
{
    BC250_DISP_IO io;
    BC250_AUX_SESSION s;
    static unsigned char buf[BC250_EDID_MAX_BYTES];
    static BC250_EDID_INFO info;
    unsigned long len = 0;

    ModelReset();
    g_m.shortReply = ShortReply;
    IoInit(&io, 200000);
    memset(buf, 0xEE, sizeof(buf));
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    CHECK(Bc250AuxReadEdid(&s, buf, sizeof(buf), &len) == BC250_DISP_STATUS_SUCCESS);
    CHECK(len == 256);
    CHECK(memcmp(buf, g_LabEdid, 256) == 0);
    CHECK(buf[256] == 0xEE);                             // nothing past the EDID's own length
    CHECK(Bc250EdidParse(buf, len, &info) == BC250_EDID_OK && strcmp(info.Name, "LEN LT2452pwC") == 0);
    // amdgpu's shape per block: 3 set-up transactions, the 16-byte reads, the stop; no segment pointer for 2 blocks.
    if (!ShortReply) CHECK(s.Transactions == 2 * 12);
    CHECK(g_m.i2cStops == 2 && g_m.segmentWrites == 0);
    CHECK(g_m.acquires == g_m.goes && g_m.releases == g_m.goes && !g_m.granted);  // per transaction, as Linux
    CHECK(g_m.readsBeforeRelease == 0 && g_m.outsideTable == 0 && io.Refusals == 0);
    CHECK(s.Defers == 0 && s.Timeouts == 0 && s.Invalid == 0 && s.Nacks == 0);
    // Every wait was a 10 us poll: about 80 polls of 10 us per transaction, inside the budget.
    CHECK(io.StalledUs >= g_m.goes * 800 && io.StalledUs <= g_m.goes * 820);
    CHECK(Bc250AuxClose(&s) == 0);
}

static void TestEdidSegments(void)
{
    BC250_DISP_IO io;
    BC250_AUX_SESSION s;
    static unsigned char buf[BC250_EDID_MAX_BYTES];
    unsigned long len = 0, i;

    // Four blocks: blocks 2 and 3 need segment 1.
    ModelReset();
    memcpy(g_m.edid, g_LabEdid, 256);
    g_m.edid[126] = 3;
    for (i = 256; i < 512; i++) g_m.edid[i] = (unsigned char)(i * 7u);
    g_m.edidLen = 512;
    IoInit(&io, 400000);
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    CHECK(Bc250AuxReadEdid(&s, buf, sizeof(buf), &len) == 0 && len == 512);
    CHECK(memcmp(buf, g_m.edid, 512) == 0);
    CHECK(g_m.segmentWrites == 2 && g_m.i2cStops == 4);
    // A buffer of two blocks: two read, no error, the rest not asked for.
    ModelReset();
    memcpy(g_m.edid, g_LabEdid, 256);
    g_m.edid[126] = 3;
    IoInit(&io, 400000);
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    CHECK(Bc250AuxReadEdid(&s, buf, 256, &len) == 0 && len == 256 && g_m.segmentWrites == 0);
    CHECK(Bc250AuxReadEdid(&s, buf, 100, &len) == BC250_DISP_STATUS_INVALID && len == 0);
}

static void TestDpcd(void)
{
    BC250_DISP_IO io;
    BC250_AUX_SESSION s;
    unsigned char b[20];
    unsigned long i;

    ModelReset();
    for (i = 0; i < sizeof(g_m.dpcd); i++) g_m.dpcd[i] = (unsigned char)(i ^ 0x5A);
    IoInit(&io, 100000);
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    CHECK(Bc250AuxDpcdRead(&s, 0x000, b, 20) == 0);     // 16 + 4
    for (i = 0; i < 20; i++) CHECK(b[i] == (unsigned char)(i ^ 0x5A));
    CHECK(s.Transactions == 2);
    // Requests the engine cannot carry.
    {
        unsigned long got;
        unsigned char big[17];
        CHECK(Bc250AuxTransfer(&s, BC250_AUX_DP_READ, 0, big, 17, &got) == BC250_DISP_STATUS_INVALID);
        CHECK(Bc250AuxTransfer(&s, BC250_AUX_DP_READ, 0x100000, big, 1, &got) == BC250_DISP_STATUS_INVALID);
        CHECK(Bc250AuxTransfer(&s, BC250_AUX_DP_READ, 0, 0, 1, &got) == BC250_DISP_STATUS_INVALID);
    }
}

// One DPCD byte with a fault script; returns the status and leaves the session in *S.
static long OneRead(enum fault F, unsigned long Count, unsigned long BudgetUs, BC250_AUX_SESSION* S, BC250_DISP_IO* Io)
{
    unsigned char b[1];
    unsigned long got = 99;
    long st;
    ModelReset();
    g_m.fault = F;
    g_m.faultCount = Count;
    IoInit(Io, BudgetUs);
    if (Bc250AuxOpen(S, Io) != 0) return -1;
    st = Bc250AuxTransfer(S, BC250_AUX_DP_READ, 0x000, b, 1, &got);
    if (st != 0) CHECK(got == 0);
    return st;
}

static void TestRetries(void)
{
    BC250_DISP_IO io;
    BC250_AUX_SESSION s;

    // DEFER: seven retries are allowed, an eighth DEFER fails; each DEFER waits 1 ms.
    CHECK(OneRead(F_DEFER, 7, 1000000, &s, &io) == 0 && s.Defers == 7 && s.Attempts == 8);
    CHECK(io.StalledUs >= 7 * 1000);
    CHECK(OneRead(F_DEFER, 8, 1000000, &s, &io) == BC250_DISP_STATUS_DEVICE && s.Defers == 8);
    CHECK(OneRead(F_I2C_DEFER, 3, 1000000, &s, &io) == 0 && s.Defers == 3);
    // Reply timeout: three retries, then the timeout status.
    CHECK(OneRead(F_RX_TIMEOUT, 3, 1000000, &s, &io) == 0 && s.Timeouts == 3);
    CHECK(OneRead(F_RX_TIMEOUT, 4, 1000000, &s, &io) == BC250_DISP_STATUS_TIMEOUT && s.Attempts == 4);
    // A reply that never comes: each wait ends at its bound, the engine is released every time.
    CHECK(OneRead(F_NEVER_DONE, 4, 1000000, &s, &io) == BC250_DISP_STATUS_TIMEOUT);
    CHECK(io.StalledUs == 4 * BC250_AUX_WAIT_MAX_US && g_m.releases == 4 && !g_m.granted);
    // Invalid replies: two retries.
    CHECK(OneRead(F_INVALID, 2, 1000000, &s, &io) == 0 && s.Invalid == 2);
    CHECK(OneRead(F_INVALID, 3, 1000000, &s, &io) == BC250_DISP_STATUS_DEVICE);
    CHECK(OneRead(F_ZERO_BYTES, 3, 1000000, &s, &io) == BC250_DISP_STATUS_DEVICE && s.Invalid == 3);
    // NACK and I2C NACK: no retry.
    CHECK(OneRead(F_NACK, 1, 1000000, &s, &io) == BC250_DISP_STATUS_DEVICE && s.Attempts == 1 && s.Nacks == 1);
    CHECK(OneRead(F_I2C_NACK, 1, 1000000, &s, &io) == BC250_DISP_STATUS_DEVICE && s.Nacks == 1);
    // HPD low: the sink is gone, no retry.
    CHECK(OneRead(F_HPD, 1, 1000000, &s, &io) == BC250_DISP_STATUS_DEVICE && s.Attempts == 1);
    CHECK(s.LastResult == BC250_AUX_HPD_DISCON);
    CHECK(g_m.releases == 1 && !g_m.granted);
}

static void TestRefusals(void)
{
    BC250_DISP_IO io;
    BC250_AUX_SESSION s;
    unsigned char b[16];
    unsigned long got, len;

    // The DMCU owns the engine: BUSY, no request sent.
    ModelReset();
    IoInit(&io, 100000);
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    g_m.dmcuOwns = 1;
    CHECK(Bc250AuxTransfer(&s, BC250_AUX_DP_READ, 0, b, 1, &got) == BC250_DISP_STATUS_BUSY);
    CHECK(g_m.goes == 0 && s.Busy == 1);
    // A stall budget too small for one transaction: the wait stops at the budget, the engine is released.
    ModelReset();
    IoInit(&io, 300);
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    CHECK(Bc250AuxTransfer(&s, BC250_AUX_DP_READ, 0, b, 1, &got) == BC250_DISP_STATUS_BUDGET);
    CHECK(io.StalledUs <= 300 && !g_m.granted);
    // A budget that runs out in the middle of an EDID read.
    ModelReset();
    IoInit(&io, 12000);
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    CHECK(Bc250AuxReadEdid(&s, b, 0, &len) == BC250_DISP_STATUS_INVALID);
    {
        static unsigned char buf[256];
        // Block 0 (12 transactions of 800 us) fits, block 1 does not: *Length says what is good.
        CHECK(Bc250AuxReadEdid(&s, buf, sizeof(buf), &len) == BC250_DISP_STATUS_BUDGET && len == 128);
        CHECK(memcmp(buf, g_LabEdid, 128) == 0);
        CHECK(io.StalledUs <= 12000 && !g_m.granted);
    }
    // A failed register write in the middle of a request: the error comes back, the engine is released.
    ModelReset();
    g_m.writeFail = BC250_REG_DMU_DP_AUX0_AUX_SW_CONTROL;
    IoInit(&io, 100000);
    CHECK(Bc250AuxOpen(&s, &io) == 0);
    CHECK(Bc250AuxTransfer(&s, BC250_AUX_DP_READ, 0, b, 1, &got) == (long)0xC0000185L);
    CHECK(!g_m.granted && g_m.goes == 0);
    // The checked accessors refuse a register outside the display table, and it never reaches the bus.
    ModelReset();
    IoInit(&io, 100000);
    {
        unsigned long v = 1;
        CHECK(Bc250DispRead(&io, BC250_REG_DMU_DP_AUX0_AUX_SW_STATUS + 0x100000ul, &v) == BC250_DISP_STATUS_ACCESS_DENIED);
        CHECK(v == 0);
        CHECK(Bc250DispWrite(&io, BC250_REG_DMU_DP_AUX0_AUX_SW_STATUS, 0) == BC250_DISP_STATUS_ACCESS_DENIED);  // read only
        CHECK(Bc250DispWrite(&io, 0, 0) == BC250_DISP_STATUS_ACCESS_DENIED);
        CHECK(io.Refusals == 3 && io.Reads == 0 && io.Writes == 0 && g_m.outsideTable == 0);
        CHECK(Bc250DispReadAllowed(BC250_REG_DMU_DP_AUX0_AUX_SW_STATUS) && !Bc250DispWriteAllowed(BC250_REG_DMU_DP_AUX0_AUX_SW_STATUS));
    }
    // Zero budget: no stall at all.
    IoInit(&io, 0);
    CHECK(Bc250DispStall(&io, 1) == BC250_DISP_STATUS_BUDGET && io.StalledUs == 0);
}

int main(void)
{
    TestOpenClose();
    TestEdidRead(0);
    TestEdidRead(5);                                    // the sink answers 5 bytes at a time
    TestEdidSegments();
    TestDpcd();
    TestRetries();
    TestRefusals();
    printf("dpaux_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
