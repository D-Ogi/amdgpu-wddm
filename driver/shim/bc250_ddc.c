// PROVENANCE: Linux AMD display DC (MIT), v6.18 7d0a66e4; dce_i2c_hw.c.
// The imported transaction bodies use checked MMIO below. Deviations: one
// DDC1 owner, 50 kHz, no HDCP polling, no DMCU takeover, positive DONE required,
// first I/O error latched and firmware pad/speed state restored on all exits.
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include "bc250_ddc.h"
#include "../kmd/edid.h"
#include "../kmd/regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"

typedef unsigned long uint32_t;
typedef unsigned char uint8_t;
typedef int bool;
#define true 1
#define false 0
#define SETUP DC_I2C_DDC1_SETUP
#define SPEED DC_I2C_DDC1_SPEED
#define HW_STATUS DC_I2C_DDC1_HW_STATUS
#define R_(r) BC250_REG_DMU_##r
#define R(r) R_(r)
#define FIELD(r, f) static const unsigned long m_##f = r##__##f##_MASK
FIELD(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_DATA_DRIVE_EN);
FIELD(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_CLK_DRIVE_EN);
FIELD(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_DATA_DRIVE_SEL);
FIELD(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_INTRA_TRANSACTION_DELAY);
FIELD(DC_I2C_DDC1_SETUP, DC_I2C_DDC1_INTRA_BYTE_DELAY);
FIELD(DC_I2C_CONTROL, DC_I2C_SOFT_RESET);
FIELD(DC_I2C_CONTROL, DC_I2C_SW_STATUS_RESET);
FIELD(DC_I2C_CONTROL, DC_I2C_SEND_RESET);
FIELD(DC_I2C_CONTROL, DC_I2C_GO);
FIELD(DC_I2C_CONTROL, DC_I2C_TRANSACTION_COUNT);
FIELD(DC_I2C_DDC1_HW_STATUS, DC_I2C_DDC1_HW_STATUS);
FIELD(DC_I2C_ARBITRATION, DC_I2C_REG_RW_CNTL_STATUS);
FIELD(DC_I2C_ARBITRATION, DC_I2C_SW_USE_I2C_REG_REQ);
FIELD(DC_I2C_ARBITRATION, DC_I2C_SW_DONE_USING_I2C_REG);
FIELD(DC_I2C_SW_STATUS, DC_I2C_SW_STATUS);
FIELD(DC_I2C_DATA, DC_I2C_DATA_RW);
FIELD(DC_I2C_DATA, DC_I2C_DATA);
FIELD(DC_I2C_DATA, DC_I2C_INDEX);
FIELD(DC_I2C_DATA, DC_I2C_INDEX_WRITE);
FIELD(DC_I2C_TRANSACTION0, DC_I2C_STOP_ON_NACK0);
FIELD(DC_I2C_TRANSACTION0, DC_I2C_START0);
FIELD(DC_I2C_TRANSACTION0, DC_I2C_RW0);
FIELD(DC_I2C_TRANSACTION0, DC_I2C_COUNT0);
FIELD(DC_I2C_TRANSACTION0, DC_I2C_STOP0);
FIELD(MICROSECOND_TIME_BASE_DIV, MICROSECOND_TIME_BASE_DIV);
FIELD(MICROSECOND_TIME_BASE_DIV, XTAL_REF_DIV);
FIELD(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_PRESCALE);
FIELD(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_THRESHOLD);
FIELD(DC_I2C_DDC1_SPEED, DC_I2C_DDC1_START_STOP_TIMING_CNTL);
#undef FIELD

enum { DC_I2C_STATUS__DC_I2C_STATUS_IDLE, DC_I2C_STATUS__DC_I2C_STATUS_USED_BY_SW,
    DC_I2C_STATUS__DC_I2C_STATUS_USED_BY_HW, DC_I2C_REG_RW_CNTL_STATUS_DMCU_ONLY = 2 };
enum { DCE_I2C_TRANSACTION_ACTION_I2C_WRITE = 0, DCE_I2C_TRANSACTION_ACTION_I2C_READ = 0x10,
    DCE_I2C_TRANSACTION_ACTION_I2C_WRITE_MOT = 0x40, I2C_CHANNEL_OPERATION_ENGINE_BUSY = 1 };
struct i2c_request_transaction_data { unsigned long action, status; uint8_t address; uint32_t length; uint8_t *data; };
struct i2c_payload { uint32_t length; uint8_t *data; };
struct dce_i2c_mask { unsigned long DC_I2C_DDC1_START_STOP_TIMING_CNTL; };
struct dce_i2c_hw {
    BC250_DDC_SESSION *session;
    const struct dce_i2c_mask *masks;
    unsigned long transaction_count, buffer_used_bytes, buffer_used_write, reference_frequency;
    long error;
};
static const struct dce_i2c_mask masks = { DC_I2C_DDC1_SPEED__DC_I2C_DDC1_START_STOP_TIMING_CNTL_MASK };

// Imported bodies cannot return an MMIO failure. Once latched, all subsequent
// accesses in that body are inert. Cleanup starts with a fresh adapter.
static unsigned long read_reg(struct dce_i2c_hw *d, unsigned long reg)
{
    unsigned long value = 0;
    if (d->error >= 0) d->error = Bc250DispRead(d->session->io, reg, &value);
    return value;
}
static unsigned long write_fields(struct dce_i2c_hw *d, unsigned long reg,
    unsigned long value, int read_first, unsigned int count, ...)
{
    va_list ap;
    unsigned int i;
    if (d->error < 0) return 0;
    if (read_first) value = read_reg(d, reg);
    va_start(ap, count);
    for (i = 0; i < count; i++) {
        unsigned long mask = va_arg(ap, unsigned long);
        unsigned long field = va_arg(ap, unsigned long);
        value = (value & ~mask) | Bc250DispField(mask, field);
    }
    va_end(ap);
    if (d->error >= 0) d->error = Bc250DispWrite(d->session->io, reg, value);
    return value;
}
static unsigned long get_field(struct dce_i2c_hw *d, unsigned long reg, unsigned long mask, void *out)
{
    unsigned long value = read_reg(d, reg);
    unsigned long field = Bc250DispGet(value, mask);
    // Linux spells two locals unsigned int and the rest uint32_t. Both are
    // 32-bit in this Windows-only shim, without pointer aliasing between them.
    memcpy(out, &field, sizeof(field));
    return value;
}
#define FN(r,f) m_##f
#define PAIR(f,v) m_##f,(unsigned long)(v)
#define REG_GET(r,f,p) get_field(dce_i2c_hw,R(r),m_##f,p)
#define REG_GET_2(r,f,p,g,q) (REG_GET(r,f,p), REG_GET(r,g,q))
#define REG_UPDATE(r,f,v) write_fields(dce_i2c_hw,R(r),0,1,1,PAIR(f,v))
#define REG_UPDATE_5(r,f,v,g,w,h,x,i,y,j,z) \
    write_fields(dce_i2c_hw,R(r),0,1,5,PAIR(f,v),PAIR(g,w),PAIR(h,x),PAIR(i,y),PAIR(j,z))
#define REG_UPDATE_N(r,n,...) write_fields(dce_i2c_hw,R(r),0,1,n,__VA_ARGS__)
#define REG_SET_2(r,b,f,v,g,w) write_fields(dce_i2c_hw,R(r),b,0,2,PAIR(f,v),PAIR(g,w))
#define REG_SET_3(r,b,f,v,g,w,h,x) write_fields(dce_i2c_hw,R(r),b,0,3,PAIR(f,v),PAIR(g,w),PAIR(h,x))
#define REG_SET_4(r,b,f,v,g,w,h,x,i,y) write_fields(dce_i2c_hw,R(r),b,0,4,PAIR(f,v),PAIR(g,w),PAIR(h,x),PAIR(i,y))
// REG_UPDATE_N values in the import are C int literals, unlike PAIR above.
// Use a fixed-arity adapter so that no va_arg ever reads an int as a long.
#undef REG_UPDATE_N
#define UPDATE_N_2(r,f,v,g,w) write_fields(dce_i2c_hw,R(r),0,1,2,f,(unsigned long)(v),g,(unsigned long)(w))
#define UPDATE_N_3(r,f,v,g,w,h,x) write_fields(dce_i2c_hw,R(r),0,1,3,f,(unsigned long)(v),g,(unsigned long)(w),h,(unsigned long)(x))
#define UPDATE_N_5(r,f,v,g,w,h,x,i,y,j,z) write_fields(dce_i2c_hw,R(r),0,1,5,f,(unsigned long)(v),g,(unsigned long)(w),h,(unsigned long)(x),i,(unsigned long)(y),j,(unsigned long)(z))
#define EXPAND_ARGS(x) x
#define REG_UPDATE_N(r,n,...) EXPAND_ARGS(UPDATE_N_##n(r,__VA_ARGS__))
#define ASSERT(x) do { if (!(x) && dce_i2c_hw->error >= 0) dce_i2c_hw->error = BC250_DISP_STATUS_BUSY; } while (0)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4244) // AMD FIFO reply: masked DATA (8 bits) to uint8_t
#endif
#include "dce_i2c_hw.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static void engine(struct dce_i2c_hw *d, BC250_DDC_SESSION *s)
{
    memset(d, 0, sizeof(*d));
    d->session = s;
    d->masks = &masks;
}
static const unsigned long transactions[4] = {
    R(DC_I2C_TRANSACTION0), R(DC_I2C_TRANSACTION1), R(DC_I2C_TRANSACTION2), R(DC_I2C_TRANSACTION3) };
#define TRY(x) do { long status_ = (x); if (status_ < 0) return status_; } while (0)
#define DDC_PAD_MASK (DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_PD_EN_MASK | \
    DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_PD_EN_MASK | DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK | \
    DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_MASK_MASK | DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_MASK_MASK)

static long prepare(BC250_DDC_SESSION *s)
{
    struct dce_i2c_hw d;
    unsigned long i, value, clock, divider;
    BC250_DISP_IO *io = s->io;
    engine(&d, s);
    TRY(Bc250DispRead(io, R(DC_I2C_ARBITRATION), &s->arbitration_before));
    if (Bc250DispGet(s->arbitration_before, m_DC_I2C_REG_RW_CNTL_STATUS) != 0)
        return BC250_DISP_STATUS_BUSY;
    if (!is_engine_available(&d)) return d.error < 0 ? d.error : BC250_DISP_STATUS_BUSY;
    TRY(d.error);
    TRY(Bc250DispRead(io, R(DC_I2C_SW_STATUS), &value));
    if (Bc250DispGet(value, m_DC_I2C_SW_STATUS) != 0) return BC250_DISP_STATUS_BUSY;
    TRY(Bc250DispRead(io, R(DP_AUX0_AUX_ARB_CONTROL), &value));
    if (Bc250DispGet(value, DP_AUX0_AUX_ARB_CONTROL__AUX_REG_RW_CNTL_STATUS_MASK) != 0)
        return BC250_DISP_STATUS_BUSY;
    TRY(Bc250DispRead(io, R(MICROSECOND_TIME_BASE_DIV), &value));
    clock = Bc250DispGet(value, m_MICROSECOND_TIME_BASE_DIV);
    divider = Bc250DispGet(value, m_XTAL_REF_DIV);
    if (!divider) divider = 2;
    // Linux falls back to BIOS crystal frequency here; we have no BIOS parser.
    // A zero base therefore refuses instead of guessing a prescaler.
    if (!clock || clock * 1000ul / divider / 50ul == 0 || clock * 1000ul / divider / 50ul > 65535ul)
        return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(io, R(DC_GPIO_DDC1_MASK), &s->mask_before));
    TRY(Bc250DispRead(io, R(DC_GPIO_AUX_CTRL_5), &s->aux_before));
    TRY(Bc250DispRead(io, R(SETUP), &s->setup_before));
    TRY(Bc250DispRead(io, R(SPEED), &s->speed_before));
    TRY(Bc250DispRead(io, R(DC_I2C_CONTROL), &s->control_before));
    for (i = 0; i < 4; i++) TRY(Bc250DispRead(io, transactions[i], &s->transaction_before[i]));
    s->saved = 1;
    // Deviation: hold controller ownership across pad changes as well, so a
    // firmware owner cannot start using the engine before we restore its mux.
    s->acquired = 1; // an acquire write can take effect even if its callback fails
    if (!acquire_engine(&d)) return d.error < 0 ? d.error : BC250_DISP_STATUS_BUSY;
    TRY(d.error);
    // hw_ddc.c MODE_I2C + hw_gpio.c GPIO_MODE_HARDWARE for both pins.
    // Deviation: bounded busy stalls replace msleep while the KMD mutex is held.
    s->pad_changed = 1;
    TRY(Bc250DispUpdate(io, R(DC_GPIO_DDC1_MASK),
        DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_PD_EN_MASK | DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_PD_EN_MASK,
        DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_PD_EN_MASK | DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_PD_EN_MASK));
    TRY(Bc250DispStall(io, 3000));
    if (s->mask_before & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK) TRY(Bc250DispStall(io, 2000));
    TRY(Bc250DispUpdate(io, R(DC_GPIO_DDC1_MASK), DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK |
        DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_MASK_MASK | DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_MASK_MASK, 0));
    TRY(Bc250DispUpdate(io, R(DC_GPIO_AUX_CTRL_5), DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK,
        DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK));
    s->engine_changed = 1;
    TRY(Bc250DispUpdate(io, R(DC_I2C_CONTROL), m_DC_I2C_SOFT_RESET, 0));
    set_speed(&d, 50);
    TRY(d.error);
    // dce_i2c_hw.c setup_engine, DCN2: DDC1 engine_id=0, limit=3, reset length=9.
    TRY(Bc250DispUpdate(io, R(DC_I2C_CONTROL), m_DC_I2C_GO | m_DC_I2C_SEND_RESET |
        m_DC_I2C_SW_STATUS_RESET | m_DC_I2C_TRANSACTION_COUNT | DC_I2C_CONTROL__DC_I2C_DDC_SELECT_MASK,
        m_DC_I2C_SW_STATUS_RESET));
    TRY(Bc250DispUpdate(io, R(SETUP), DC_I2C_DDC1_SETUP__DC_I2C_DDC1_TIME_LIMIT_MASK |
        DC_I2C_DDC1_SETUP__DC_I2C_DDC1_SEND_RESET_LENGTH_MASK | DC_I2C_DDC1_SETUP__DC_I2C_DDC1_ENABLE_MASK,
        Bc250DispField(DC_I2C_DDC1_SETUP__DC_I2C_DDC1_TIME_LIMIT_MASK, 3) |
        Bc250DispField(DC_I2C_DDC1_SETUP__DC_I2C_DDC1_SEND_RESET_LENGTH_MASK, 9) |
        DC_I2C_DDC1_SETUP__DC_I2C_DDC1_ENABLE_MASK));
    TRY(Bc250DispUpdate(io, R(DC_I2C_ARBITRATION), DC_I2C_ARBITRATION__DC_I2C_NO_QUEUED_SW_GO_MASK, 0));
    s->open = 1;
    return 0;
}

long bc250_ddc_open(BC250_DDC_SESSION *s, BC250_DISP_IO *io)
{
    long status;
    if (!s || !io) return BC250_DISP_STATUS_INVALID;
    memset(s, 0, sizeof(*s));
    s->io = io;
    status = prepare(s);
    if (status < 0) {
        s->first_failure = status;
        (void)bc250_ddc_close(s);
    }
    return status;
}

long bc250_ddc_close(BC250_DDC_SESSION *s)
{
    long first = 0, status;
    unsigned long owner = 0, value, i;
    struct dce_i2c_hw d;
    BC250_DISP_IO *io;
    if (!s || !s->io) return BC250_DISP_STATUS_INVALID;
    io = s->io;
    s->open = 0;
#define CLEAN(x) do { status = (x); if (status < 0 && first >= 0) first = status; } while (0)
    if (s->acquired) {
        CLEAN(Bc250DispRead(io, R(DC_I2C_ARBITRATION), &owner));
        if (status >= 0 && Bc250DispGet(owner, m_DC_I2C_REG_RW_CNTL_STATUS) == 1) {
            if (s->engine_changed) {
                // AMD release_engine: reset only a SW transaction, never a HW one.
                CLEAN(Bc250DispRead(io, R(DC_I2C_SW_STATUS), &value));
                if (status >= 0) CLEAN(Bc250DispUpdate(io, R(DC_I2C_CONTROL),
                    m_DC_I2C_SOFT_RESET | m_DC_I2C_SW_STATUS_RESET,
                    m_DC_I2C_SW_STATUS_RESET | (Bc250DispGet(value, m_DC_I2C_SW_STATUS) == 1 ? m_DC_I2C_SOFT_RESET : 0)));
                // Restore the settings we own, not GO/status/reset strobes.
                for (i = 0; i < 4; i++) CLEAN(Bc250DispWrite(io, transactions[i], s->transaction_before[i]));
                CLEAN(Bc250DispWrite(io, R(SPEED), s->speed_before));
                CLEAN(Bc250DispWrite(io, R(SETUP), s->setup_before));
                CLEAN(Bc250DispUpdate(io, R(DC_I2C_CONTROL), DC_I2C_CONTROL__DC_I2C_DDC_SELECT_MASK |
                    m_DC_I2C_TRANSACTION_COUNT | m_DC_I2C_SOFT_RESET, s->control_before));
                CLEAN(Bc250DispUpdate(io, R(DC_I2C_ARBITRATION), DC_I2C_ARBITRATION__DC_I2C_NO_QUEUED_SW_GO_MASK,
                    s->arbitration_before));
            }
            if (s->pad_changed && s->saved) {
                CLEAN(Bc250DispUpdate(io, R(DC_GPIO_AUX_CTRL_5), DC_GPIO_AUX_CTRL_5__DDC_PAD1_I2CMODE_MASK, s->aux_before));
                CLEAN(Bc250DispUpdate(io, R(DC_GPIO_DDC1_MASK), DDC_PAD_MASK, s->mask_before));
                s->pad_changed = 0;
            }
            CLEAN(Bc250DispUpdate(io, R(DC_I2C_ARBITRATION), m_DC_I2C_SW_DONE_USING_I2C_REG, m_DC_I2C_SW_DONE_USING_I2C_REG));
            engine(&d, s);
            cntl_stuck_hw_workaround(&d);
            CLEAN(d.error);
        } else if (status >= 0 && (s->pad_changed || Bc250DispGet(owner, m_DC_I2C_REG_RW_CNTL_STATUS) != 0)) {
            if (first >= 0) first = BC250_DISP_STATUS_BUSY;
        }
        s->acquired = 0;
    }
    // If ownership vanished or could not be read, leave the new owner's pins
    // alone. The cleanup failure is returned and recorded, never hidden.
#undef CLEAN
    s->cleanup_failure = first;
    return first;
}

static long payload(struct dce_i2c_hw *d, unsigned char address, unsigned long action,
                     unsigned char *data, unsigned long length)
{
    struct i2c_request_transaction_data request;
    request.action = action; request.status = 0;
    request.address = (unsigned char)((address << 1) | ((action & DCE_I2C_TRANSACTION_ACTION_I2C_READ) != 0));
    request.length = length; request.data = data;
    (void)process_transaction(d, &request);
    if (d->error < 0) return d->error;
    return request.status ? BC250_DISP_STATUS_BUSY : 0;
}

static long transfer(BC250_DDC_SESSION *s, unsigned char address, unsigned char offset,
                      unsigned char *out, unsigned long length, int segment)
{
    struct dce_i2c_hw d;
    struct i2c_payload reply;
    unsigned char seg = (unsigned char)segment;
    unsigned long waited;
    long status;
    if (!s->open || !length || length > 128 || !out) return BC250_DISP_STATUS_INVALID;
    engine(&d, s);
    TRY(Bc250DispUpdate(s->io, R(DC_I2C_CONTROL), m_DC_I2C_SW_STATUS_RESET, m_DC_I2C_SW_STATUS_RESET));
    if (segment >= 0) TRY(payload(&d, 0x30, DCE_I2C_TRANSACTION_ACTION_I2C_WRITE_MOT, &seg, 1));
    TRY(payload(&d, address, DCE_I2C_TRANSACTION_ACTION_I2C_WRITE_MOT, &offset, 1));
    TRY(payload(&d, address, DCE_I2C_TRANSACTION_ACTION_I2C_READ, out, length));
    execute_transaction(&d);
    TRY(d.error);
    s->transactions++;
    for (waited = 0; ; waited += 10) {
        TRY(Bc250DispRead(s->io, R(DC_I2C_SW_STATUS), &s->status));
        if (s->status & DC_I2C_SW_STATUS__DC_I2C_SW_STOPPED_ON_NACK_MASK) {
            s->nacks++; return BC250_DISP_STATUS_DEVICE;
        }
        if (s->status & (DC_I2C_SW_STATUS__DC_I2C_SW_TIMEOUT_MASK | DC_I2C_SW_STATUS__DC_I2C_SW_ABORTED_MASK |
            DC_I2C_SW_STATUS__DC_I2C_SW_BUFFER_OVERFLOW_MASK)) return BC250_DISP_STATUS_DEVICE;
        if (Bc250DispGet(s->status, m_DC_I2C_SW_STATUS) == 0 && (s->status & DC_I2C_SW_STATUS__DC_I2C_SW_DONE_MASK)) break;
        // 128 bytes at 50 kHz take ~24 ms. Bound includes the E-DDC addressing.
        if (waited >= 35000) { s->timeouts++; return BC250_DISP_STATUS_TIMEOUT; }
        status = Bc250DispStall(s->io, 10);
        if (status < 0) return status;
    }
    reply.length = length; reply.data = out;
    process_channel_reply(&d, &reply);
    return d.error;
}

long bc250_ddc_read_edid(BC250_DDC_SESSION *s, unsigned char *bytes, unsigned long capacity, unsigned long *length)
{
    unsigned long block, wanted = BC250_EDID_BLOCK;
    long status;
    if (!s || !bytes || !length || capacity < BC250_EDID_BLOCK) return BC250_DISP_STATUS_INVALID;
    *length = 0;
    for (block = 0; block * BC250_EDID_BLOCK < wanted; block++) {
        status = transfer(s, 0x50, (unsigned char)((block & 1) * BC250_EDID_BLOCK),
            bytes + block * BC250_EDID_BLOCK, BC250_EDID_BLOCK, block < 2 ? -1 : (int)(block / 2));
        if (status < 0) { s->first_failure = status; return status; }
        *length += BC250_EDID_BLOCK;
        if (!block) {
            if (!Bc250EdidHeaderValid(bytes) || !Bc250EdidChecksumValid(bytes)) return BC250_DISP_STATUS_MISMATCH;
            wanted = Bc250EdidTotalBytes(bytes);
            if (wanted > capacity) wanted = capacity / BC250_EDID_BLOCK * BC250_EDID_BLOCK;
        }
    }
    return 0;
}

long bc250_ddc_read_adapter(BC250_DDC_SESSION *s, BC250_DUAL_MODE_ADAPTER *adapter)
{
    unsigned char bytes[32] = {0};
    long status;
    if (!s || !adapter) return BC250_DISP_STATUS_INVALID;
    // A failed adapter query must not prevent reading the monitor's EDID.
    status = transfer(s, 0x40, 0, bytes, sizeof(bytes), -1);
    bc250_ddc_decode_adapter(bytes, status < 0 ? 0 : sizeof(bytes), adapter);
    adapter->status = status;
    return status;
}
