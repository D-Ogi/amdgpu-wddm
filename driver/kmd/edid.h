// The monitor's EDID: the parser, and the list of desktop source modes that display.c offers from it. Plain C and
// plain integers with no WDK header, so that driver\kmd\test\edid_test.c compiles this file as the miniport does.
//
// The layout is VESA E-EDID 1.4 (base block) and CTA-861 (extension tag 0x02). The parser takes only what the
// driver uses: the identity (manufacturer, product, monitor name), the preferred detailed timing, the sizes of the
// detailed, standard and established timings and of the CTA video data block, the range limits, and the audio data
// (short audio descriptors and the speaker allocation) that DP audio step 4 programs into the Azalia endpoint.
// docs/design/display-modes.md has the design; modeset.c is the owner in the miniport.
#pragma once

#define BC250_EDID_BLOCK 128ul
#define BC250_EDID_MAX_BLOCKS 4ul                       // the base block and up to three extensions are cached
#define BC250_EDID_MAX_BYTES (BC250_EDID_BLOCK * BC250_EDID_MAX_BLOCKS)
#define BC250_EDID_MAX_MODES 48ul
#define BC250_EDID_MAX_SADS 16ul
#define BC250_EDID_NAME_CHARS 13ul                      // a display descriptor holds 13 characters

// Bc250EdidParse's verdict. OK and EXTENSION_DROPPED leave a usable base block; every other value means no EDID.
enum bc250_edid_reason {
    BC250_EDID_OK = 0,
    BC250_EDID_EXTENSION_DROPPED,       // the base block is good; an extension had a bad checksum or was missing
    BC250_EDID_SHORT,                   // fewer than 128 bytes
    BC250_EDID_HEADER,                  // the 8-byte header is not 00 FF FF FF FF FF FF 00
    BC250_EDID_CHECKSUM,                // the base block does not sum to 0 modulo 256
    BC250_EDID_VERSION,                 // not EDID structure version 1
    BC250_EDID_REASON_COUNT
};

// Where a mode came from: a bit each, because the same size can come from several places.
#define BC250_MODE_SRC_NATIVE 0x01u     // the timing the firmware lit (always the first entry of a mode list)
#define BC250_MODE_SRC_DTD 0x02u        // a detailed timing descriptor (base block or CTA extension)
#define BC250_MODE_SRC_STD 0x04u        // a standard timing
#define BC250_MODE_SRC_EST 0x08u        // an established timing
#define BC250_MODE_SRC_VIC 0x10u        // a CTA short video descriptor
#define BC250_MODE_SRC_COMMON 0x20u     // the driver's list of common desktop sizes

typedef struct _BC250_EDID_TIMING {
    unsigned long PixelClock10Khz;      // 10 kHz units, as the descriptor holds it
    unsigned long HActive, HBlank, HSyncOffset, HSyncWidth;
    unsigned long VActive, VBlank, VSyncOffset, VSyncWidth;
    unsigned long Flags;                // byte 17 of the descriptor (bit 7: interlaced)
} BC250_EDID_TIMING;

typedef struct _BC250_EDID_MODE {
    unsigned long Width, Height, RefreshHz;
    unsigned long Source;               // BC250_MODE_SRC_*
} BC250_EDID_MODE;

// A CTA-861 short audio descriptor, decoded: format code (1 = LPCM), channels (count, not count - 1), the
// sample-rate bit field (bit 0 = 32 kHz .. bit 6 = 192 kHz) and the third byte (LPCM: the sample-size bits).
typedef struct _BC250_EDID_SAD {
    unsigned char Format, Channels, Rates, Byte2;
} BC250_EDID_SAD;

typedef struct _BC250_EDID_INFO {
    unsigned long Reason;               // enum bc250_edid_reason
    unsigned long Blocks;               // blocks that passed their checks (1 + good extensions)
    unsigned long ExtensionsDeclared;   // byte 126 of the base block
    unsigned long Version, Revision;
    unsigned char ManufacturerId[2];    // bytes 8 and 9 as they are on the wire (PnP ID, big endian, 5 bits a letter)
    char Vendor[4];                     // the three letters of the PnP ID
    unsigned long ProductCode;          // bytes 10 and 11, little endian
    int HasName;
    char Name[BC250_EDID_NAME_CHARS + 1];       // the monitor name descriptor (0xFC), trailing LF and blanks cut
    int HasRange;
    unsigned long MinVHz, MaxVHz, MinHKhz, MaxHKhz, MaxPixelClockMhz;
    int HasPreferred;
    BC250_EDID_TIMING Preferred;        // the first detailed timing of the base block
    unsigned long PreferredRefreshMilliHz;
    unsigned long ModeCount;
    BC250_EDID_MODE Modes[BC250_EDID_MAX_MODES];
    unsigned long CtaBlocks;
    unsigned long SadCount;
    BC250_EDID_SAD Sads[BC250_EDID_MAX_SADS];
    int HasSpeaker;
    unsigned char Speaker;              // the first byte of the speaker allocation data block (bit 0 = FL/FR)
} BC250_EDID_INFO;

int Bc250EdidHeaderValid(const unsigned char* Block);
int Bc250EdidChecksumValid(const unsigned char* Block);
// How many bytes an EDID of this base block occupies, from its extension count and capped at BC250_EDID_MAX_BYTES.
unsigned long Bc250EdidTotalBytes(const unsigned char* Base);
// Parses Length bytes. Info is always filled (zeroed first); the return value is Info->Reason.
unsigned long Bc250EdidParse(const unsigned char* Bytes, unsigned long Length, BC250_EDID_INFO* Info);
const char* Bc250EdidReasonText(unsigned long Reason);

// ---- the desktop source modes ------------------------------------------------------------------------------------
//
// Every mode is scanned out at the native timing through the scaler, so a source mode must fit inside the native
// active area: no source mode is wider or taller than the native one (the DCN 2.0.1 scaler can also shrink, but
// downscaling needs a higher DPP clock and other DCHUB request settings, which stage A does not program). A mode is
// at least 640x480. The list holds the native mode first, then the sizes the EDID lists, then the common desktop
// sizes, without duplicates, in descending order of width and then height after the native entry.
#define BC250_MODE_LIST_MAX 24ul
#define BC250_MODE_MIN_WIDTH 640ul
#define BC250_MODE_MIN_HEIGHT 480ul

// The start-latched switch EnableDisplayModes (modeset.c): what each level offers.
#define BC250_DISPLAY_MODES_OFF 0ul     // the driver before this feature: no AUX, no descriptor, one mode
#define BC250_DISPLAY_MODES_CENTERED 1ul// EDID and descriptor; smaller modes centered 1:1 (no scaler ratio)
#define BC250_DISPLAY_MODES_SCALED 2ul  // the default: smaller modes scaled (aspect ratio kept, stretched, centered)

typedef struct _BC250_MODE_LIST {
    unsigned long Count;
    BC250_EDID_MODE Modes[BC250_MODE_LIST_MAX];
} BC250_MODE_LIST;

// Builds the list for a native active size. Edid may be NULL (no EDID read). Level BC250_DISPLAY_MODES_OFF gives the
// native mode alone. Returns List->Count; 0 only for a native size of 0.
unsigned long Bc250ModeListBuild(unsigned long NativeWidth, unsigned long NativeHeight, const BC250_EDID_INFO* Edid,
                                 unsigned long Level, BC250_MODE_LIST* List);
// Nonzero when Width x Height is on the list.
int Bc250ModeListHas(const BC250_MODE_LIST* List, unsigned long Width, unsigned long Height);
