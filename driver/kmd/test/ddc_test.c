// Exercise the production I2C engine with register failures, NACK and timeouts.
// The fake models FIFO indexing, arbitration and DONE, not the driver's writes.
#include <stdio.h>
#include <string.h>
#include "../../shim/bc250_ddc.h"
#include "../edid.h"
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "edid_lg_redacted.h"
#define R(r) BC250_REG_DMU_##r
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failed++; } } while (0)
static int failed;
typedef struct model {
    unsigned long reg[0x8000], initial[0x8000];
    unsigned char fifo[512], edid[512], adapter[32];
    unsigned long index, position, calls, fail_at, writes, aux_writes, gos;
    int hang, nack;
} MODEL;
static MODEL model;
static long rd(void *context, unsigned long offset, unsigned long *value)
{
    MODEL *m = context;
    if (++m->calls == m->fail_at) return BC250_DISP_STATUS_DEVICE;
    if (offset == R(DC_I2C_DATA)) {
        *value = Bc250DispField(DC_I2C_DATA__DC_I2C_DATA_MASK, m->fifo[m->index++]);
    } else *value = m->reg[offset / 4];
    return 0;
}
static long wr(void *context, unsigned long offset, unsigned long value)
{
    MODEL *m = context;
    unsigned long old = m->reg[offset / 4];
    if (++m->calls == m->fail_at) return BC250_DISP_STATUS_DEVICE;
    m->writes++;
    if (offset == R(DP_AUX0_AUX_SW_CONTROL) || offset == R(DP_AUX0_AUX_SW_DATA)) m->aux_writes++;
    if (offset == R(DC_I2C_DATA)) {
        if (value & DC_I2C_DATA__DC_I2C_INDEX_WRITE_MASK) m->index = Bc250DispGet(value, DC_I2C_DATA__DC_I2C_INDEX_MASK);
        if (!(value & DC_I2C_DATA__DC_I2C_DATA_RW_MASK))
            m->fifo[m->index++] = (unsigned char)Bc250DispGet(value, DC_I2C_DATA__DC_I2C_DATA_MASK);
    } else if (offset == R(DC_I2C_ARBITRATION)) {
        if (value & DC_I2C_ARBITRATION__DC_I2C_SW_DONE_USING_I2C_REG_MASK) value = 0;
        else if (value & DC_I2C_ARBITRATION__DC_I2C_SW_USE_I2C_REG_REQ_MASK)
            value |= Bc250DispField(DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK, 1);
    } else if (offset == R(DC_I2C_CONTROL)) {
        if (value & DC_I2C_CONTROL__DC_I2C_SW_STATUS_RESET_MASK) m->reg[R(DC_I2C_SW_STATUS)/4] = 0;
        if ((value & DC_I2C_CONTROL__DC_I2C_GO_MASK) && !(old & DC_I2C_CONTROL__DC_I2C_GO_MASK)) {
            unsigned long n, pos = 0, seg = 0, off = 0, start;
            unsigned long count = Bc250DispGet(value, DC_I2C_CONTROL__DC_I2C_TRANSACTION_COUNT_MASK) + 1;
            static const unsigned long tx[] = {R(DC_I2C_TRANSACTION0), R(DC_I2C_TRANSACTION1), R(DC_I2C_TRANSACTION2)};
            m->gos++;
            for (n = 0; n < count; n++) {
                unsigned char addr = m->fifo[pos++];
                unsigned long len = Bc250DispGet(m->reg[tx[n]/4], DC_I2C_TRANSACTION0__DC_I2C_COUNT0_MASK);
                if (!(addr & 1)) {
                    if ((addr >> 1) == 0x30) seg = m->fifo[pos];
                    else off = m->fifo[pos];
                    pos += len;
                } else {
                    start = pos;
                    memcpy(m->fifo + start, (addr >> 1) == 0x40 ? m->adapter : m->edid + seg * 256 + off, len);
                }
            }
            m->reg[R(DC_I2C_SW_STATUS)/4] = m->hang ? 1 : (m->nack ? DC_I2C_SW_STATUS__DC_I2C_SW_STOPPED_ON_NACK_MASK
                : DC_I2C_SW_STATUS__DC_I2C_SW_DONE_MASK);
            value &= ~DC_I2C_CONTROL__DC_I2C_GO_MASK;
        }
    }
    m->reg[offset / 4] = value;
    return 0;
}
static void stall(void *ctx, unsigned long us) { (void)ctx; (void)us; }
static void checksum(unsigned char *p)
{
    unsigned long i, sum = 0;
    for (i = 0; i < 127; i++) sum += p[i];
    p[127] = (unsigned char)(0u - sum);
}
static void reset(BC250_DISP_IO *io)
{
    static const unsigned char header[] = {0,255,255,255,255,255,255,0};
    unsigned long i;
    memset(&model, 0, sizeof(model));
    memset(io, 0, sizeof(*io));
    io->Context = &model; io->Read = rd; io->Write = wr; io->Stall = stall; io->BudgetUs = 200000;
    model.reg[R(MICROSECOND_TIME_BASE_DIV)/4] = Bc250DispField(MICROSECOND_TIME_BASE_DIV__MICROSECOND_TIME_BASE_DIV_MASK, 100);
    model.reg[R(DC_GPIO_DDC1_MASK)/4] = DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK;
    memcpy(model.edid, header, sizeof(header));
    model.edid[18] = 1; model.edid[19] = 4; model.edid[126] = 3;
    for (i = 0; i < 4; i++) checksum(model.edid + i * 128);
    memcpy(model.adapter, "DP-HDMI ADAPTOR", 15); model.adapter[15] = 4;
    model.adapter[16] = 0xA0; model.adapter[29] = 121; // 302.5 MHz, not 302
    memcpy(model.initial, model.reg, sizeof(model.reg));
}
static void check_restored(void)
{
    CHECK(model.reg[R(DC_GPIO_DDC1_MASK)/4] == model.initial[R(DC_GPIO_DDC1_MASK)/4]);
    CHECK(model.reg[R(DC_GPIO_AUX_CTRL_5)/4] == model.initial[R(DC_GPIO_AUX_CTRL_5)/4]);
    CHECK(model.reg[R(DC_I2C_DDC1_SPEED)/4] == model.initial[R(DC_I2C_DDC1_SPEED)/4]);
    CHECK(model.reg[R(DC_I2C_DDC1_SETUP)/4] == model.initial[R(DC_I2C_DDC1_SETUP)/4]);
    CHECK(Bc250DispGet(model.reg[R(DC_I2C_ARBITRATION)/4], DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK) == 0);
    CHECK(model.aux_writes == 0);
}
int main(void)
{
    BC250_DISP_IO io;
    BC250_DDC_SESSION session;
    BC250_DUAL_MODE_ADAPTER adapter;
    BC250_EDID_INFO info;
    unsigned char bytes[512];
    unsigned long length, i, count;
    reset(&io);
    CHECK(bc250_ddc_open(&session, &io) == 0);
    CHECK(bc250_ddc_read_edid(&session, bytes, sizeof(bytes), &length) == 0);
    CHECK(length == 512 && memcmp(bytes, model.edid, 512) == 0);
    CHECK(bc250_ddc_read_adapter(&session, &adapter) == 0);
    CHECK(adapter.type == BC250_ADAPTER_TYPE2_HDMI && adapter.max_tmds_khz == 302500);
    count = model.calls;
    CHECK(bc250_ddc_close(&session) == 0); check_restored();
    // Every I/O failure before cleanup: no unchecked MMIO; best-effort teardown
    // preserves the first failure and releases the shared controller/pads.
    for (i = 1; i <= count; i++) {
        long status;
        reset(&io); model.fail_at = i;
        status = bc250_ddc_open(&session, &io);
        if (status >= 0) {
            status = bc250_ddc_read_edid(&session, bytes, sizeof(bytes), &length);
            if (status >= 0) (void)bc250_ddc_read_adapter(&session, &adapter);
            CHECK(bc250_ddc_close(&session) == 0);
        }
        check_restored();
    }
    reset(&io); model.reg[R(DC_I2C_ARBITRATION)/4] = Bc250DispField(DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK, 2);
    CHECK(bc250_ddc_open(&session, &io) == BC250_DISP_STATUS_BUSY); CHECK(model.writes == 0);
    reset(&io); model.reg[R(MICROSECOND_TIME_BASE_DIV)/4] = 0;
    CHECK(bc250_ddc_open(&session, &io) == BC250_DISP_STATUS_MISMATCH); CHECK(model.writes == 0);
    reset(&io); CHECK(bc250_ddc_open(&session, &io) == 0); model.hang = 1;
    CHECK(bc250_ddc_read_edid(&session, bytes, sizeof(bytes), &length) == BC250_DISP_STATUS_TIMEOUT);
    CHECK(io.StalledUs <= 40000); CHECK(bc250_ddc_close(&session) == 0); check_restored();
    reset(&io); CHECK(bc250_ddc_open(&session, &io) == 0); model.nack = 1;
    CHECK(bc250_ddc_read_adapter(&session, &adapter) < 0 && adapter.type == BC250_ADAPTER_UNKNOWN);
    model.nack = 0;
    CHECK(bc250_ddc_read_edid(&session, bytes, sizeof(bytes), &length) == 0);
    CHECK(bc250_ddc_close(&session) == 0); check_restored();
    reset(&io); io.BudgetUs = 0;
    CHECK(bc250_ddc_open(&session, &io) == BC250_DISP_STATUS_BUDGET); check_restored();
    reset(&io); CHECK(bc250_ddc_open(&session, &io) == 0);
    model.reg[R(DC_I2C_ARBITRATION)/4] = Bc250DispField(DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK, 2);
    i = model.writes;
    CHECK(bc250_ddc_close(&session) == BC250_DISP_STATUS_BUSY);
    CHECK(model.writes == i); // a foreign owner is never reset or re-muxed
    reset(&io);
    model.adapter[16] = 0; bc250_ddc_decode_adapter(model.adapter, 32, &adapter);
    CHECK(adapter.type == BC250_ADAPTER_TYPE1_HDMI && adapter.max_tmds_khz == 165000);
    model.adapter[0] = '!'; bc250_ddc_decode_adapter(model.adapter, 32, &adapter);
    CHECK(adapter.type == BC250_ADAPTER_UNKNOWN);
    // HDMI VSDB is distinct from CTA audio support; DVI with a CTA block stays DVI.
    model.edid[126] = 1; checksum(model.edid);
    model.edid[128] = 2; model.edid[129] = 3; model.edid[130] = 12; model.edid[131] = 0x40;
    model.edid[132] = 0x67; model.edid[133] = 3; model.edid[134] = 12; model.edid[139] = 60;
    checksum(model.edid + 128);
    CHECK(Bc250EdidParse(model.edid, 256, &info) == BC250_EDID_OK);
    CHECK(info.Hdmi && info.BasicAudio && info.MaxTmdsClockKhz == 300000);
    model.edid[132] = 0x64; checksum(model.edid + 128);
    (void)Bc250EdidParse(model.edid, 256, &info); CHECK(!info.Hdmi);
    model.edid[132] = 0x67; model.edid[255]++; (void)Bc250EdidParse(model.edid, 256, &info);
    CHECK(!info.Hdmi && !info.BasicAudio);
    CHECK(Bc250EdidParse(g_LgBaseEdid, sizeof(g_LgBaseEdid), &info) == BC250_EDID_EXTENSION_DROPPED);
    CHECK(info.HasPreferred && info.Preferred.HActive == 1920 && info.Preferred.VActive == 1080);
    CHECK(info.Preferred.PixelClock10Khz == 22880 && !info.Hdmi && !info.BasicAudio);
    printf("DDC/adapter/HDMI EDID: %d failures, %lu injected I/O failures\n", failed, count);
    return failed != 0;
}
