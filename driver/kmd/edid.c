// The monitor's EDID and the desktop source modes: see edid.h. Plain C, no WDK header, no CRT call, so that the
// miniport and driver\kmd\test\edid_test.c compile the same file.
//
// The byte layout follows the published VESA E-EDID 1.4 and CTA-861 standards. No AMD code is involved: amdgpu hands
// the EDID to the DRM core, and this parser is our own, checked against the lab monitor's EDID that amdgpu read in
// the E03 trace (evidence/linux/2026-09-21-E03-init-trace/amdgpu-events.txt) and against hand-built samples.
#include "edid.h"

static const unsigned char g_Header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

int Bc250EdidHeaderValid(const unsigned char* Block)
{
    unsigned long i;
    for (i = 0; i < 8; i++)
        if (Block[i] != g_Header[i]) return 0;
    return 1;
}

int Bc250EdidChecksumValid(const unsigned char* Block)
{
    unsigned long i;
    unsigned char sum = 0;
    for (i = 0; i < BC250_EDID_BLOCK; i++) sum = (unsigned char)(sum + Block[i]);
    return sum == 0;
}

unsigned long Bc250EdidTotalBytes(const unsigned char* Base)
{
    unsigned long blocks = 1ul + Base[126];
    if (blocks > BC250_EDID_MAX_BLOCKS) blocks = BC250_EDID_MAX_BLOCKS;
    return blocks * BC250_EDID_BLOCK;
}

const char* Bc250EdidReasonText(unsigned long Reason)
{
    switch (Reason) {
    case BC250_EDID_OK: return "ok";
    case BC250_EDID_EXTENSION_DROPPED: return "base block ok, an extension dropped";
    case BC250_EDID_SHORT: return "shorter than one block";
    case BC250_EDID_HEADER: return "bad header";
    case BC250_EDID_CHECKSUM: return "bad base block checksum";
    case BC250_EDID_VERSION: return "not EDID version 1";
    default: return "?";
    }
}

static void AddMode(BC250_EDID_INFO* Info, unsigned long Width, unsigned long Height, unsigned long RefreshHz,
                    unsigned long Source)
{
    unsigned long i;
    if (Width == 0 || Height == 0) return;
    for (i = 0; i < Info->ModeCount; i++) {
        BC250_EDID_MODE* m = &Info->Modes[i];
        if (m->Width == Width && m->Height == Height && m->RefreshHz == RefreshHz) { m->Source |= Source; return; }
    }
    if (Info->ModeCount >= BC250_EDID_MAX_MODES) return;
    Info->Modes[Info->ModeCount].Width = Width;
    Info->Modes[Info->ModeCount].Height = Height;
    Info->Modes[Info->ModeCount].RefreshHz = RefreshHz;
    Info->Modes[Info->ModeCount].Source = Source;
    Info->ModeCount++;
}

// An 18-byte detailed timing descriptor (pixel clock not 0).
static void DecodeDtd(const unsigned char* d, BC250_EDID_TIMING* t)
{
    t->PixelClock10Khz = (unsigned long)d[0] | ((unsigned long)d[1] << 8);
    t->HActive = (unsigned long)d[2] | (((unsigned long)d[4] & 0xF0u) << 4);
    t->HBlank = (unsigned long)d[3] | (((unsigned long)d[4] & 0x0Fu) << 8);
    t->VActive = (unsigned long)d[5] | (((unsigned long)d[7] & 0xF0u) << 4);
    t->VBlank = (unsigned long)d[6] | (((unsigned long)d[7] & 0x0Fu) << 8);
    t->HSyncOffset = (unsigned long)d[8] | (((unsigned long)d[11] & 0xC0u) << 2);
    t->HSyncWidth = (unsigned long)d[9] | (((unsigned long)d[11] & 0x30u) << 4);
    t->VSyncOffset = ((unsigned long)d[10] >> 4) | (((unsigned long)d[11] & 0x0Cu) << 2);
    t->VSyncWidth = ((unsigned long)d[10] & 0x0Fu) | (((unsigned long)d[11] & 0x03u) << 4);
    t->Flags = d[17];
}

static unsigned long RefreshMilliHz(const BC250_EDID_TIMING* t)
{
    unsigned long long total = (unsigned long long)(t->HActive + t->HBlank) * (t->VActive + t->VBlank);
    if (total == 0) return 0;
    return (unsigned long)(((unsigned long long)t->PixelClock10Khz * 10000ull * 1000ull + total / 2) / total);
}

static void AddDtd(BC250_EDID_INFO* Info, const unsigned char* d, int First)
{
    BC250_EDID_TIMING t;
    unsigned long milli;
    DecodeDtd(d, &t);
    if (t.Flags & 0x80u) return;                // interlaced: not a desktop size this driver offers
    milli = RefreshMilliHz(&t);
    AddMode(Info, t.HActive, t.VActive, (milli + 500) / 1000, BC250_MODE_SRC_DTD);
    if (First && !Info->HasPreferred) {
        Info->HasPreferred = 1;
        Info->Preferred = t;
        Info->PreferredRefreshMilliHz = milli;
    }
}

// A display descriptor's 13 text bytes: up to the first LF, trailing blanks cut, non-printing bytes as '?'.
static void CopyText(const unsigned char* Text, char* Out)
{
    unsigned long n = 0, i;
    for (i = 0; i < BC250_EDID_NAME_CHARS && Text[i] != 0x0A && Text[i] != 0; i++)
        Out[n++] = (Text[i] >= 0x20 && Text[i] < 0x7F) ? (char)Text[i] : '?';
    while (n > 0 && Out[n - 1] == ' ') n--;
    Out[n] = 0;
}

static void ParseDescriptor(BC250_EDID_INFO* Info, const unsigned char* d)
{
    if (d[0] != 0 || d[1] != 0 || d[2] != 0) return;
    switch (d[3]) {
    case 0xFC:                                  // monitor name
        if (!Info->HasName) { CopyText(d + 5, Info->Name); Info->HasName = Info->Name[0] != 0; }
        break;
    case 0xFD:                                  // range limits (EDID 1.4 offset flags in byte 4 are not applied)
        Info->HasRange = 1;
        Info->MinVHz = d[5]; Info->MaxVHz = d[6]; Info->MinHKhz = d[7]; Info->MaxHKhz = d[8];
        Info->MaxPixelClockMhz = (unsigned long)d[9] * 10ul;
        break;
    default:                                    // the serial string (0xFF) and the rest are not used
        break;
    }
}

// VESA established timings I and II (bytes 35 and 36) and the manufacturer's timing bit of byte 37, in bit order
// from bit 7 of byte 35. 1024x768 at 87 Hz is interlaced and left out (width 0).
static const unsigned short g_Established[17][3] = {
    { 720, 400, 70 }, { 720, 400, 88 }, { 640, 480, 60 }, { 640, 480, 67 }, { 640, 480, 72 }, { 640, 480, 75 },
    { 800, 600, 56 }, { 800, 600, 60 }, { 800, 600, 72 }, { 800, 600, 75 }, { 832, 624, 75 }, { 0, 0, 0 },
    { 1024, 768, 60 }, { 1024, 768, 70 }, { 1024, 768, 75 }, { 1280, 1024, 75 }, { 1152, 870, 75 } };

// A standard timing pair (bytes 38-53, and the 0xFA descriptor which this parser does not read). 0x01 0x01 and
// 0x00 0x00 are unused slots.
static void AddStandard(BC250_EDID_INFO* Info, unsigned char b0, unsigned char b1)
{
    unsigned long w, h;
    if ((b0 == 0x01 && b1 == 0x01) || b0 == 0x00) return;
    w = ((unsigned long)b0 + 31ul) * 8ul;
    switch (b1 >> 6) {
    case 0: h = (Info->Version == 1 && Info->Revision < 3) ? w : w * 10ul / 16ul; break;     // 1:1 before 1.3, 16:10
    case 1: h = w * 3ul / 4ul; break;                                                       // 4:3
    case 2: h = w * 4ul / 5ul; break;                                                       // 5:4
    default: h = w * 9ul / 16ul; break;                                                     // 16:9
    }
    AddMode(Info, w, h, (unsigned long)(b1 & 0x3Fu) + 60ul, BC250_MODE_SRC_STD);
}

// CTA-861 video identification codes of progressive, square-pixel formats: VIC, width, height, refresh. Codes that
// are interlaced, pixel-repeated or non-square (720x480, 720x576) are not listed and are skipped.
static const unsigned short g_Vic[][4] = {
    { 1, 640, 480, 60 }, { 4, 1280, 720, 60 }, { 16, 1920, 1080, 60 }, { 19, 1280, 720, 50 }, { 31, 1920, 1080, 50 },
    { 32, 1920, 1080, 24 }, { 33, 1920, 1080, 25 }, { 34, 1920, 1080, 30 }, { 60, 1280, 720, 24 },
    { 61, 1280, 720, 25 }, { 62, 1280, 720, 30 }, { 63, 1920, 1080, 120 }, { 64, 1920, 1080, 100 },
    { 93, 3840, 2160, 24 }, { 94, 3840, 2160, 25 }, { 95, 3840, 2160, 30 }, { 96, 3840, 2160, 50 },
    { 97, 3840, 2160, 60 } };

static void AddVic(BC250_EDID_INFO* Info, unsigned long Vic)
{
    unsigned long i;
    for (i = 0; i < sizeof(g_Vic) / sizeof(g_Vic[0]); i++)
        if (g_Vic[i][0] == Vic) { AddMode(Info, g_Vic[i][1], g_Vic[i][2], g_Vic[i][3], BC250_MODE_SRC_VIC); return; }
}

static void ParseCta(BC250_EDID_INFO* Info, const unsigned char* b)
{
    unsigned long dtdStart = b[2], i, k;

    Info->CtaBlocks++;
    if (dtdStart > 127 || (dtdStart != 0 && dtdStart < 4)) return;
    // Data block collection, revision 3 and later: from byte 4 up to the first detailed timing.
    if (b[1] >= 3 && dtdStart >= 4) {
        for (i = 4; i < dtdStart; ) {
            unsigned long tag = b[i] >> 5, len = b[i] & 0x1Fu;
            const unsigned char* p = b + i + 1;
            if (i + 1 + len > dtdStart) break;          // a block that runs past the collection: stop
            if (tag == 1) {                             // audio: short audio descriptors, 3 bytes each
                for (k = 0; k + 3 <= len && Info->SadCount < BC250_EDID_MAX_SADS; k += 3) {
                    BC250_EDID_SAD* s = &Info->Sads[Info->SadCount];
                    s->Format = (unsigned char)((p[k] >> 3) & 0x0Fu);
                    s->Channels = (unsigned char)((p[k] & 0x07u) + 1u);
                    s->Rates = (unsigned char)(p[k + 1] & 0x7Fu);
                    s->Byte2 = p[k + 2];
                    if (s->Format != 0) Info->SadCount++;
                }
            } else if (tag == 2) {                      // video: short video descriptors
                for (k = 0; k < len; k++) {
                    unsigned long svd = p[k];
                    AddVic(Info, (svd >= 129 && svd <= 192) ? (svd & 0x7Fu) : svd);     // bit 7 = native for 1-64
                }
            } else if (tag == 4 && len >= 1 && !Info->HasSpeaker) {         // speaker allocation
                Info->HasSpeaker = 1;
                Info->Speaker = p[0];
            }
            i += 1 + len;
        }
    }
    // Detailed timings of the extension, up to a pixel clock of 0 or the checksum byte.
    if (dtdStart >= 4)
        for (i = dtdStart; i + 18 <= 127; i += 18) {
            if (b[i] == 0 && b[i + 1] == 0) break;
            AddDtd(Info, b + i, 0);
        }
}

unsigned long Bc250EdidParse(const unsigned char* Bytes, unsigned long Length, BC250_EDID_INFO* Info)
{
    unsigned char* clear = (unsigned char*)Info;
    unsigned long i, ext, blocks;

    for (i = 0; i < sizeof(*Info); i++) clear[i] = 0;
    if (Bytes == 0 || Length < BC250_EDID_BLOCK) return Info->Reason = BC250_EDID_SHORT;
    if (!Bc250EdidHeaderValid(Bytes)) return Info->Reason = BC250_EDID_HEADER;
    if (!Bc250EdidChecksumValid(Bytes)) return Info->Reason = BC250_EDID_CHECKSUM;
    Info->Version = Bytes[18];
    Info->Revision = Bytes[19];
    if (Info->Version != 1) return Info->Reason = BC250_EDID_VERSION;
    Info->Blocks = 1;
    Info->Reason = BC250_EDID_OK;

    Info->ManufacturerId[0] = Bytes[8];
    Info->ManufacturerId[1] = Bytes[9];
    {
        unsigned long id = ((unsigned long)Bytes[8] << 8) | Bytes[9];
        Info->Vendor[0] = (char)('@' + ((id >> 10) & 0x1Fu));
        Info->Vendor[1] = (char)('@' + ((id >> 5) & 0x1Fu));
        Info->Vendor[2] = (char)('@' + (id & 0x1Fu));
        Info->Vendor[3] = 0;
    }
    Info->ProductCode = (unsigned long)Bytes[10] | ((unsigned long)Bytes[11] << 8);

    // The four 18-byte descriptors (54..125): the first, when it is a timing, is the preferred one (EDID 1.4 makes
    // that mandatory; 1.3 says it in byte 24 bit 1).
    for (i = 0; i < 4; i++) {
        const unsigned char* d = Bytes + 54 + 18 * i;
        if (d[0] != 0 || d[1] != 0) AddDtd(Info, d, i == 0);
        else ParseDescriptor(Info, d);
    }
    for (i = 0; i < 17; i++) {
        unsigned long byte = 35 + i / 8, bit = 7 - (i % 8);
        if ((Bytes[byte] >> bit) & 1u)
            AddMode(Info, g_Established[i][0], g_Established[i][1], g_Established[i][2], BC250_MODE_SRC_EST);
    }
    for (i = 0; i < 8; i++) AddStandard(Info, Bytes[38 + 2 * i], Bytes[39 + 2 * i]);

    Info->ExtensionsDeclared = Bytes[126];
    blocks = 1ul + Info->ExtensionsDeclared;
    if (blocks > BC250_EDID_MAX_BLOCKS) { blocks = BC250_EDID_MAX_BLOCKS; Info->Reason = BC250_EDID_EXTENSION_DROPPED; }
    for (ext = 1; ext < blocks; ext++) {
        const unsigned char* b = Bytes + ext * BC250_EDID_BLOCK;
        if ((ext + 1) * BC250_EDID_BLOCK > Length || !Bc250EdidChecksumValid(b)) {
            Info->Reason = BC250_EDID_EXTENSION_DROPPED;
            continue;
        }
        Info->Blocks++;
        if (b[0] == 0x02) ParseCta(Info, b);
    }
    return Info->Reason;
}

// ---- the desktop source modes ------------------------------------------------------------------------------------

// Common desktop sizes, offered when they fit the native mode even when the EDID does not list them: the 16:9,
// 16:10, 4:3 and 5:4 sizes that Windows and games ask for most.
static const unsigned short g_Common[][2] = {
    { 1920, 1080 }, { 1680, 1050 }, { 1600, 900 }, { 1440, 900 }, { 1400, 1050 }, { 1280, 1024 }, { 1280, 960 },
    { 1280, 800 }, { 1280, 720 }, { 1152, 864 }, { 1024, 768 }, { 800, 600 }, { 640, 480 } };

int Bc250ModeListHas(const BC250_MODE_LIST* List, unsigned long Width, unsigned long Height)
{
    unsigned long i;
    for (i = 0; i < List->Count; i++)
        if (List->Modes[i].Width == Width && List->Modes[i].Height == Height) return 1;
    return 0;
}

static void ListAdd(BC250_MODE_LIST* List, unsigned long NativeWidth, unsigned long NativeHeight, unsigned long Width,
                    unsigned long Height, unsigned long RefreshHz, unsigned long Source)
{
    unsigned long i;
    if (Width > NativeWidth || Height > NativeHeight) return;       // downscaling is not offered (edid.h)
    if (Width < BC250_MODE_MIN_WIDTH || Height < BC250_MODE_MIN_HEIGHT) return;
    for (i = 0; i < List->Count; i++)
        if (List->Modes[i].Width == Width && List->Modes[i].Height == Height) { List->Modes[i].Source |= Source; return; }
    if (List->Count >= BC250_MODE_LIST_MAX) return;
    List->Modes[List->Count].Width = Width;
    List->Modes[List->Count].Height = Height;
    List->Modes[List->Count].RefreshHz = RefreshHz;
    List->Modes[List->Count].Source = Source;
    List->Count++;
}

unsigned long Bc250ModeListBuild(unsigned long NativeWidth, unsigned long NativeHeight, const BC250_EDID_INFO* Edid,
                                 unsigned long Level, BC250_MODE_LIST* List)
{
    unsigned long i, j;

    List->Count = 0;
    if (NativeWidth == 0 || NativeHeight == 0) return 0;
    List->Modes[0].Width = NativeWidth;
    List->Modes[0].Height = NativeHeight;
    List->Modes[0].RefreshHz = 0;               // the native timing's refresh is the inherited signal's, not ours
    List->Modes[0].Source = BC250_MODE_SRC_NATIVE;
    List->Count = 1;
    if (Level == BC250_DISPLAY_MODES_OFF) return List->Count;
    if (Edid != 0 && (Edid->Reason == BC250_EDID_OK || Edid->Reason == BC250_EDID_EXTENSION_DROPPED))
        for (i = 0; i < Edid->ModeCount; i++)
            ListAdd(List, NativeWidth, NativeHeight, Edid->Modes[i].Width, Edid->Modes[i].Height,
                    Edid->Modes[i].RefreshHz, Edid->Modes[i].Source);
    for (i = 0; i < sizeof(g_Common) / sizeof(g_Common[0]); i++)
        ListAdd(List, NativeWidth, NativeHeight, g_Common[i][0], g_Common[i][1], 0, BC250_MODE_SRC_COMMON);
    // Insertion sort of the entries after the native one: width, then height, both descending.
    for (i = 2; i < List->Count; i++) {
        BC250_EDID_MODE m = List->Modes[i];
        for (j = i; j > 1 && (List->Modes[j - 1].Width < m.Width ||
                              (List->Modes[j - 1].Width == m.Width && List->Modes[j - 1].Height < m.Height)); j--)
            List->Modes[j] = List->Modes[j - 1];
        List->Modes[j] = m;
    }
    return List->Count;
}
