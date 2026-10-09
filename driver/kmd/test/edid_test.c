// Host test of edid.c: the EDID parser and the desktop source mode list (display modes, stage A). Built with no WDK
// header by run_modeset.ps1. Positive controls: the lab monitor's EDID (redacted, edid_lab_redacted.h) and a
// hand-built 4K TV EDID with a CTA block of several audio formats. Negative controls: every reason the parser can
// refuse a block, damaged and missing extensions, a data block collection that runs past its end, and a mode list
// that would overflow.
#include <stdio.h>
#include <string.h>
#include "../edid.h"
#include "edid_lab_redacted.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void FixChecksum(unsigned char* Block)
{
    unsigned long i;
    unsigned char sum = 0;
    for (i = 0; i < 127; i++) sum = (unsigned char)(sum + Block[i]);
    Block[127] = (unsigned char)(0x100u - sum);
}

static const BC250_EDID_MODE* FindMode(const BC250_EDID_INFO* Info, unsigned long W, unsigned long H, unsigned long Hz)
{
    unsigned long i;
    for (i = 0; i < Info->ModeCount; i++)
        if (Info->Modes[i].Width == W && Info->Modes[i].Height == H && Info->Modes[i].RefreshHz == Hz)
            return &Info->Modes[i];
    return 0;
}

static int HasSize(const BC250_EDID_INFO* Info, unsigned long W, unsigned long H)
{
    unsigned long i;
    for (i = 0; i < Info->ModeCount; i++)
        if (Info->Modes[i].Width == W && Info->Modes[i].Height == H) return 1;
    return 0;
}

static void TestLabMonitor(void)
{
    static BC250_EDID_INFO info;
    static BC250_MODE_LIST list;
    static const unsigned short expected[][2] = {
        { 1920, 1200 }, { 1920, 1080 }, { 1680, 1050 }, { 1600, 1200 }, { 1600, 900 }, { 1440, 900 }, { 1400, 1050 },
        { 1280, 1024 }, { 1280, 960 }, { 1280, 800 }, { 1280, 720 }, { 1152, 864 }, { 1024, 768 }, { 800, 600 },
        { 720, 480 }, { 640, 480 } };
    const BC250_EDID_MODE* m;
    unsigned long i;

    CHECK(Bc250EdidHeaderValid(g_LabEdid));
    CHECK(Bc250EdidChecksumValid(g_LabEdid));
    CHECK(Bc250EdidChecksumValid(g_LabEdid + 128));
    CHECK(Bc250EdidTotalBytes(g_LabEdid) == 256);
    CHECK(Bc250EdidParse(g_LabEdid, sizeof(g_LabEdid), &info) == BC250_EDID_OK);
    CHECK(info.Blocks == 2 && info.ExtensionsDeclared == 1 && info.CtaBlocks == 1);
    CHECK(info.Version == 1 && info.Revision == 4);
    CHECK(strcmp(info.Vendor, "LEN") == 0);
    CHECK(info.ManufacturerId[0] == 0x30 && info.ManufacturerId[1] == 0xAE);
    CHECK(info.ProductCode == 0x1144);
    CHECK(info.HasName && strcmp(info.Name, "LEN LT2452pwC") == 0);
    // The serial descriptor is never taken as the name, redacted or not.
    CHECK(strstr(info.Name, "XXX") == 0);
    CHECK(info.HasRange && info.MinVHz == 50 && info.MaxVHz == 75 && info.MinHKhz == 30 && info.MaxHKhz == 75);
    CHECK(info.MaxPixelClockMhz == 170);
    // The preferred timing: 1920x1200, CVT reduced blanking, 154 MHz, the timing the firmware lights.
    CHECK(info.HasPreferred);
    CHECK(info.Preferred.PixelClock10Khz == 15400);
    CHECK(info.Preferred.HActive == 1920 && info.Preferred.HBlank == 160);
    CHECK(info.Preferred.VActive == 1200 && info.Preferred.VBlank == 35);
    CHECK(info.Preferred.HSyncOffset == 48 && info.Preferred.HSyncWidth == 32);
    CHECK(info.Preferred.VSyncOffset == 3 && info.Preferred.VSyncWidth == 6);
    CHECK(info.PreferredRefreshMilliHz == 59950);
    m = FindMode(&info, 1920, 1200, 60);
    CHECK(m != 0 && (m->Source & BC250_MODE_SRC_DTD));
    // Standard timings: 1280x1024 at 60 and 72, 1440x900 (16:10 since EDID 1.3) at 60 and 75, 1680x1050, 1600x1200,
    // 1920x1080 (16:9), and the odd 640x480 at 66 Hz of slot 1.
    CHECK(FindMode(&info, 1280, 1024, 60) != 0 && FindMode(&info, 1280, 1024, 72) != 0);
    CHECK(FindMode(&info, 1440, 900, 60) != 0 && FindMode(&info, 1440, 900, 75) != 0);
    CHECK(FindMode(&info, 1680, 1050, 60) != 0 && FindMode(&info, 1600, 1200, 60) != 0);
    CHECK(FindMode(&info, 640, 480, 66) != 0);
    m = FindMode(&info, 1920, 1080, 60);
    CHECK(m != 0 && (m->Source & BC250_MODE_SRC_STD) && (m->Source & BC250_MODE_SRC_VIC) && (m->Source & BC250_MODE_SRC_DTD));
    // Established timings: 720x400@70, 640x480@60/72/75, 800x600@60/72/75, 1024x768@60/70/75, 1280x1024@75.
    CHECK(FindMode(&info, 720, 400, 70) != 0 && FindMode(&info, 720, 400, 88) == 0);
    CHECK(FindMode(&info, 800, 600, 56) == 0 && FindMode(&info, 800, 600, 75) != 0);
    CHECK(FindMode(&info, 1024, 768, 70) != 0 && FindMode(&info, 1280, 1024, 75) != 0);
    CHECK(FindMode(&info, 832, 624, 75) == 0);
    // CTA: VIC 1, 4, 16 (sent as 144 = native flag), 19, 31; 2, 3, 17, 18 are not square-pixel and are skipped.
    CHECK(FindMode(&info, 1280, 720, 50) != 0 && FindMode(&info, 1920, 1080, 50) != 0);
    CHECK(!HasSize(&info, 720, 576));
    // The extension's detailed timings: 1080p60, 720p60, 480p (720x480) and 1080p50.
    m = FindMode(&info, 720, 480, 60);
    CHECK(m != 0 && m->Source == BC250_MODE_SRC_DTD);
    // Audio: one LPCM descriptor, 2 channels, 32-192 kHz, 16/20/24 bit; speaker allocation FL/FR.
    CHECK(info.SadCount == 1);
    CHECK(info.Sads[0].Format == 1 && info.Sads[0].Channels == 2 && info.Sads[0].Rates == 0x7F && info.Sads[0].Byte2 == 0x07);
    CHECK(info.HasSpeaker && info.Speaker == 0x01);

    // The mode list of the lab's 1920x1200: native first, then descending, nothing larger, nothing below 640x480.
    CHECK(Bc250ModeListBuild(1920, 1200, &info, BC250_DISPLAY_MODES_SCALED, &list) == sizeof(expected) / sizeof(expected[0]));
    for (i = 0; i < list.Count && i < sizeof(expected) / sizeof(expected[0]); i++)
        CHECK(list.Modes[i].Width == expected[i][0] && list.Modes[i].Height == expected[i][1]);
    CHECK(list.Modes[0].Source & BC250_MODE_SRC_NATIVE);
    CHECK(list.Modes[0].Source & BC250_MODE_SRC_DTD);                       // the EDID lists the native size too
    CHECK(Bc250ModeListHas(&list, 1600, 900) && (list.Modes[4].Source == BC250_MODE_SRC_COMMON));
    CHECK(!Bc250ModeListHas(&list, 720, 400));
    // Every size the brief names is on the list.
    CHECK(Bc250ModeListHas(&list, 1920, 1080) && Bc250ModeListHas(&list, 1680, 1050) && Bc250ModeListHas(&list, 1440, 900));
    CHECK(Bc250ModeListHas(&list, 1280, 1024) && Bc250ModeListHas(&list, 1280, 720));
    // Level 1 offers the same sizes (centered); level 0 the native mode alone, as before this feature.
    CHECK(Bc250ModeListBuild(1920, 1200, &info, BC250_DISPLAY_MODES_CENTERED, &list) == 16);
    CHECK(Bc250ModeListBuild(1920, 1200, &info, BC250_DISPLAY_MODES_OFF, &list) == 1);
    CHECK(list.Modes[0].Width == 1920 && list.Modes[0].Height == 1200);
    // A smaller native size drops every larger mode.
    CHECK(Bc250ModeListBuild(1280, 1024, &info, BC250_DISPLAY_MODES_SCALED, &list) > 1);
    for (i = 0; i < list.Count; i++) CHECK(list.Modes[i].Width <= 1280 && list.Modes[i].Height <= 1024);
    CHECK(list.Modes[0].Width == 1280 && list.Modes[0].Height == 1024);
    // No EDID: the native mode and the common sizes that fit.
    CHECK(Bc250ModeListBuild(1920, 1200, 0, BC250_DISPLAY_MODES_SCALED, &list) == 14);
    CHECK(!Bc250ModeListHas(&list, 1600, 1200) && Bc250ModeListHas(&list, 1600, 900));
    CHECK(Bc250ModeListBuild(0, 1200, &info, BC250_DISPLAY_MODES_SCALED, &list) == 0);
}

// A 4K TV, EDID 1.3: preferred 3840x2160@60, a 13-character name without LF, range limits, an interlaced detailed
// timing (skipped), and a CTA block with LPCM 8 channels, AC-3 and a 7-speaker allocation.
static void BuildTv(unsigned char* e)
{
    static const unsigned char header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    static const unsigned char dtd4k[18] = { 0x08, 0xE8, 0x00, 0x30, 0xF2, 0x70, 0x5A, 0x80, 0xB0, 0x58, 0x8A, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x1E };
    static const unsigned char dtd1080i[18] = { 0x01, 0x1D, 0x80, 0x18, 0x71, 0x1C, 0x16, 0x20, 0x58, 0x2C, 0x25, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00, 0x9E };
    static const unsigned char dtd720[18] = { 0x01, 0x1D, 0x00, 0x72, 0x51, 0xD0, 0x1E, 0x20, 0x6E, 0x28, 0x55, 0x00,
                                              0x06, 0x44, 0x21, 0x00, 0x00, 0x1E };
    static const unsigned char cta[] = { 0x02, 0x03, 20, 0x70,
                                         0x44, 0x61, 0x90, 0x04, 0x05,                      // video: 97, 16, 4, 5
                                         0x26, 0x0F, 0x7F, 0x07, 0x15, 0x07, 0x50,          // audio: LPCM 8ch, AC-3 6ch
                                         0x83, 0x7F, 0x00, 0x00 };                          // speakers
    unsigned char* d;

    memset(e, 0, 256);
    memcpy(e, header, 8);
    e[8] = 0x52; e[9] = 0x74;                       // "TST"
    e[10] = 0x42; e[11] = 0x00;
    e[18] = 1; e[19] = 3;
    e[35] = 0x20;                                   // 640x480@60
    e[38] = 0xD1; e[39] = 0xC0;                     // 1920x1080@60
    for (d = e + 40; d < e + 54; d++) *d = 0x01;
    memcpy(e + 54, dtd4k, 18);
    d = e + 72; d[3] = 0xFC; memcpy(d + 5, "BC250 TEST TV", 13);
    d = e + 90; d[3] = 0xFD; d[5] = 24; d[6] = 60; d[7] = 15; d[8] = 135; d[9] = 60; d[10] = 0x00; d[11] = 0x0A;
    memcpy(e + 108, dtd1080i, 18);
    e[126] = 1;
    FixChecksum(e);
    memcpy(e + 128, cta, sizeof(cta));
    memcpy(e + 128 + 20, dtd720, 18);
    FixChecksum(e + 128);
}

static void TestTv(void)
{
    static unsigned char e[256];
    static BC250_EDID_INFO info;
    static BC250_MODE_LIST list;
    const BC250_EDID_MODE* m;

    BuildTv(e);
    CHECK(Bc250EdidParse(e, sizeof(e), &info) == BC250_EDID_OK);
    CHECK(strcmp(info.Vendor, "TST") == 0 && info.ProductCode == 0x0042);
    CHECK(info.HasName && strcmp(info.Name, "BC250 TEST TV") == 0);
    CHECK(info.HasPreferred && info.Preferred.HActive == 3840 && info.Preferred.VActive == 2160);
    CHECK(info.PreferredRefreshMilliHz == 60000);
    CHECK(info.HasRange && info.MaxPixelClockMhz == 600);
    CHECK(!HasSize(&info, 1920, 540));                  // the interlaced timing is not a mode
    m = FindMode(&info, 3840, 2160, 60);
    CHECK(m != 0 && (m->Source & BC250_MODE_SRC_DTD) && (m->Source & BC250_MODE_SRC_VIC));
    CHECK(FindMode(&info, 1280, 720, 60) != 0);
    CHECK(info.SadCount == 2);
    CHECK(info.Sads[0].Format == 1 && info.Sads[0].Channels == 8 && info.Sads[0].Rates == 0x7F);
    CHECK(info.Sads[1].Format == 2 && info.Sads[1].Channels == 6 && info.Sads[1].Byte2 == 0x50);
    CHECK(info.HasSpeaker && info.Speaker == 0x7F);
    CHECK(Bc250ModeListBuild(3840, 2160, &info, BC250_DISPLAY_MODES_SCALED, &list) == 14);
    CHECK(list.Modes[0].Width == 3840 && (list.Modes[0].Source & BC250_MODE_SRC_VIC));
    CHECK(list.Modes[1].Width == 1920 && list.Modes[1].Height == 1080);

    // EDID 1.2: a standard timing of aspect code 0 is 1:1, not 16:10.
    e[19] = 2; e[38] = 0x81; e[39] = 0x00;
    FixChecksum(e);
    CHECK(Bc250EdidParse(e, sizeof(e), &info) == BC250_EDID_OK);
    CHECK(FindMode(&info, 1280, 1280, 60) != 0 && !HasSize(&info, 1280, 800));
}

static void TestNegative(void)
{
    static unsigned char e[512];
    static BC250_EDID_INFO info;

    // Too short, no buffer.
    CHECK(Bc250EdidParse(g_LabEdid, 127, &info) == BC250_EDID_SHORT && info.ModeCount == 0);
    CHECK(Bc250EdidParse(0, 256, &info) == BC250_EDID_SHORT);
    // A bad header byte.
    memcpy(e, g_LabEdid, 256); e[7] = 0x01;
    CHECK(Bc250EdidParse(e, 256, &info) == BC250_EDID_HEADER && !info.HasName);
    // A flipped bit anywhere in the base block.
    memcpy(e, g_LabEdid, 256); e[60] ^= 0x10;
    CHECK(Bc250EdidParse(e, 256, &info) == BC250_EDID_CHECKSUM && info.ModeCount == 0);
    // EDID structure version 2 (a valid checksum).
    memcpy(e, g_LabEdid, 256); e[18] = 2; FixChecksum(e);
    CHECK(Bc250EdidParse(e, 256, &info) == BC250_EDID_VERSION && !info.HasPreferred);
    // A damaged extension: the base block stays, the CTA data does not.
    memcpy(e, g_LabEdid, 256); e[200] ^= 0x01;
    CHECK(Bc250EdidParse(e, 256, &info) == BC250_EDID_EXTENSION_DROPPED);
    CHECK(info.Blocks == 1 && info.SadCount == 0 && !info.HasSpeaker && info.CtaBlocks == 0);
    CHECK(info.HasName && info.HasPreferred && !HasSize(&info, 720, 480));
    // An extension announced but not read.
    CHECK(Bc250EdidParse(g_LabEdid, 128, &info) == BC250_EDID_EXTENSION_DROPPED && info.Blocks == 1);
    // More extensions than the cache holds: three are read, the rest dropped.
    memcpy(e, g_LabEdid, 256); e[126] = 5; FixChecksum(e);
    memcpy(e + 256, g_LabEdid + 128, 128); memcpy(e + 384, g_LabEdid + 128, 128);
    CHECK(Bc250EdidTotalBytes(e) == 512);
    CHECK(Bc250EdidParse(e, 512, &info) == BC250_EDID_EXTENSION_DROPPED && info.Blocks == 4 && info.CtaBlocks == 3);
    CHECK(info.SadCount == 3);
    // A data block that claims to run past the collection: parsing stops there, the DTDs still count.
    memcpy(e, g_LabEdid, 256); e[128 + 4] = 0x5F; FixChecksum(e + 128);         // video, 31 bytes
    CHECK(Bc250EdidParse(e, 256, &info) == BC250_EDID_OK && info.SadCount == 0 && FindMode(&info, 720, 480, 60) != 0);
    // A DTD offset past the block: no data blocks, no DTDs, no crash.
    memcpy(e, g_LabEdid, 256); e[128 + 2] = 200; FixChecksum(e + 128);
    CHECK(Bc250EdidParse(e, 256, &info) == BC250_EDID_OK && info.SadCount == 0 && FindMode(&info, 720, 480, 60) == 0);
    // An extension that is not CTA is counted as read and otherwise ignored.
    memcpy(e, g_LabEdid, 256); e[128] = 0x70; FixChecksum(e + 128);
    CHECK(Bc250EdidParse(e, 256, &info) == BC250_EDID_OK && info.Blocks == 2 && info.CtaBlocks == 0);
}

static void TestListOverflow(void)
{
    static BC250_EDID_INFO info;
    static BC250_MODE_LIST list;
    unsigned long i;

    memset(&info, 0, sizeof(info));
    info.Reason = BC250_EDID_OK;
    for (i = 0; i < BC250_EDID_MAX_MODES; i++) {
        info.Modes[i].Width = 700 + 20 * i;
        info.Modes[i].Height = 500 + 10 * i;
        info.Modes[i].Source = BC250_MODE_SRC_STD;
    }
    info.ModeCount = BC250_EDID_MAX_MODES;
    CHECK(Bc250ModeListBuild(3840, 2160, &info, BC250_DISPLAY_MODES_SCALED, &list) == BC250_MODE_LIST_MAX);
    CHECK(list.Modes[0].Width == 3840 && list.Modes[0].Source == BC250_MODE_SRC_NATIVE);
    for (i = 2; i < list.Count; i++)
        CHECK(list.Modes[i - 1].Width > list.Modes[i].Width ||
              (list.Modes[i - 1].Width == list.Modes[i].Width && list.Modes[i - 1].Height > list.Modes[i].Height));
    // An EDID the parser refused gives no modes even when its mode table is filled.
    info.Reason = BC250_EDID_CHECKSUM;
    CHECK(Bc250ModeListBuild(3840, 2160, &info, BC250_DISPLAY_MODES_SCALED, &list) == 14);
}

int main(void)
{
    TestLabMonitor();
    TestTv();
    TestNegative();
    TestListOverflow();
    printf("edid_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
