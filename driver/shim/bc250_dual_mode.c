// PROVENANCE: AMD DC (MIT), Linux v6.18, link_detection.c query_dp_dual_mode_adaptor.
// The import decodes a captured register bank; the caller owns bounded I2C.
#include <string.h>
#include "bc250_ddc.h"
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned long uint32_t;
typedef int bool;
#define true 1
#define false 0
#define DP_ADAPTOR_TYPE2_SIZE 0x20
#define DP_ADAPTOR_TYPE2_REG_ID 0x10
#define DP_ADAPTOR_TYPE2_REG_MAX_TMDS_CLK 0x1D
#define DP_ADAPTOR_TYPE2_ID 0xA0
#define DP_ADAPTOR_TYPE2_MAX_TMDS_CLK 600
#define DP_ADAPTOR_TYPE2_MIN_TMDS_CLK 25
#define DP_ADAPTOR_DVI_MAX_TMDS_CLK 165000
#define DP_ADAPTOR_HDMI_SAFE_MAX_TMDS_CLK 165000
#define DP_HDMI_DONGLE_ADDRESS 0x40
#define DP_HDMI_DONGLE_SIGNATURE_EOT 0x04
struct dp_hdmi_dongle_signature_data { int8_t id[15]; uint8_t eot; };
static const uint8_t dp_hdmi_dongle_signature_str[] = "DP-HDMI ADAPTOR";
enum display_dongle_type { DISPLAY_DONGLE_NONE, DISPLAY_DONGLE_DP_DVI_DONGLE,
    DISPLAY_DONGLE_DP_HDMI_DONGLE, DISPLAY_DONGLE_DP_HDMI_MISMATCHED_DONGLE };
struct dc_link { int unused; };
struct ddc_service { struct dc_link *link; const uint8_t *bytes; };
struct display_sink_capability { enum display_dongle_type dongle_type; uint32_t max_hdmi_pixel_clock; bool is_dongle_type_one; };
static bool i2c_read(struct ddc_service *ddc, uint32_t address, uint8_t *out, uint32_t size)
{
    if (address != DP_HDMI_DONGLE_ADDRESS || size != 32) return false;
    memcpy(out, ddc->bytes, size);
    return true;
}
#define CONN_DATA_DETECT(source,...) ((void)(source), (void)link)
#include "link_detection.inc"

void bc250_ddc_decode_adapter(const unsigned char *bytes, unsigned long length, BC250_DUAL_MODE_ADAPTER *adapter)
{
    struct ddc_service ddc;
    struct display_sink_capability caps;
    unsigned long i;
    int signature = 1;
    memset(adapter, 0, sizeof(*adapter));
    if (!bytes || length < sizeof(adapter->bytes)) return;
    memcpy(adapter->bytes, bytes, sizeof(adapter->bytes));
    ddc.link = 0; ddc.bytes = bytes;
    memset(&caps, 0, sizeof(caps));
    query_dp_dual_mode_adaptor(&ddc, &caps);
    // Deviation: AMD's type-1 loop ignores all character mismatches. Do not
    // identify random I2C data as HDMI from EOT alone; allow its subversion byte.
    for (i = 0; i < sizeof(dp_hdmi_dongle_signature_str) - 1; i++)
        if (i != 3 && bytes[i] != dp_hdmi_dongle_signature_str[i]) signature = 0;
    if (bytes[15] != DP_HDMI_DONGLE_SIGNATURE_EOT) signature = 0;
    if (bytes[DP_ADAPTOR_TYPE2_REG_ID] == DP_ADAPTOR_TYPE2_ID) {
        unsigned long max = (unsigned long)bytes[DP_ADAPTOR_TYPE2_REG_MAX_TMDS_CLK] * 2500ul;
        if (max < DP_ADAPTOR_TYPE2_MIN_TMDS_CLK * 1000ul || max > DP_ADAPTOR_TYPE2_MAX_TMDS_CLK * 1000ul) return;
        adapter->type = signature ? BC250_ADAPTER_TYPE2_HDMI : BC250_ADAPTER_TYPE2_OTHER;
        // Deviation: retain the half-MHz part lost by AMD's integer-MHz intermediate.
        adapter->max_tmds_khz = max;
    } else if (signature && caps.is_dongle_type_one) {
        adapter->type = BC250_ADAPTER_TYPE1_HDMI;
        adapter->max_tmds_khz = caps.max_hdmi_pixel_clock;
    }
}
