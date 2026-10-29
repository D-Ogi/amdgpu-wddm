#ifndef BC250_DCN_CURSOR_SHAPE_H
#define BC250_DCN_CURSOR_SHAPE_H

// The hardware pointer of pipe 0 (dcn_cursor.c, docs/design/hardware-pointer.md): the conversion of a WDDM pointer
// shape into the DCN 2.0.1 cursor surface, and the placement of that surface on the plane. Plain integers and no WDK
// header, like display_rotation.h, so that the host test (test/dcn_cursor_test.c, gate "dcn-cursor") compiles this
// text and not a copy of it. No allocation, no lock, no floating point: the placement runs under a spin lock.
//
// The contracts used here:
//   - DXGKARG_SETPOINTERSHAPE (d3dkmddi.h, WDK 10.0.26100): Monochrome is a 1 bpp AND mask of Width x Height
//     followed by a 1 bpp XOR mask of the same size, each row Pitch bytes; Color is 32 bpp ARGB; MaskedColor is
//     32 bpp ARGB with the mask in the alpha byte, 0 (the RGB value replaces the screen pixel) or 0xFF (the RGB
//     value is XORed with the screen pixel). Rows run top to bottom, and in a 1 bpp row the most significant bit
//     of a byte is the leftmost pixel (the DIB rule).
//   - DXGKARG_SETPOINTERPOSITION (the comment of d3dkmddi.h): X and Y are the column and the row of the top left
//     pixel of the pointer image, from the top left pixel of the VidPN source.
//   - The DCN cursor (AMD DC, MIT: hubp2_cursor_set_attributes, hubp2_cursor_set_position, hubp2_get_lines_per_chunk,
//     dpp2_set_cursor_attributes, dc_hw_types.h enum dc_cursor_color_format). The cursor is in plane coordinates:
//     CURSOR_X/Y_POSITION less CURSOR_HOT_SPOT_X/Y is the top left pixel, and the hot spot fields are 8 bits. AMD's
//     display manager gives a cursor that starts left of or above the plane a position of 0 and moves the part that
//     is outside into the hot spot (amdgpu_dm get_cursor_position); this code does the same.
//   - The scan-out pipe does not rotate (display_rotation.h, BD-247). A rotated source is drawn rotated by DWM into
//     the physical primary, so the pointer image is rotated here and its position mapped to the physical frame.

// The size reported in DXGK_DRIVERCAPS.MaxPointerWidth/Height. DCN 2.0's cursor takes 256 (CURSOR_WIDTH is 9 bits,
// CURSOR_PITCH has a 256 setting, hubp2_get_lines_per_chunk has a row for 129 to 256, dcn20 caps max_cursor_size
// 256), and the 8-bit hot spot fields cover a 256 pixel image exactly.
#define BC250_CURSOR_MAX_SIZE 256u
#define BC250_CURSOR_PITCH_PIXELS 256u                                          // CURSOR_PITCH 2
#define BC250_CURSOR_BUFFER_BYTES (BC250_CURSOR_PITCH_PIXELS * BC250_CURSOR_MAX_SIZE * 4u)    // 256 KiB
#define BC250_CURSOR_RAW_MAX_BYTES (256u * 1024u)                                 // the copy of one input shape

// DXGK_POINTERFLAGS (d3dkmddi.h).
#define BC250_POINTER_MONOCHROME 0x1u
#define BC250_POINTER_COLOR 0x2u
#define BC250_POINTER_MASKED_COLOR 0x4u
#define BC250_POINTER_CAPS (BC250_POINTER_MONOCHROME | BC250_POINTER_COLOR | BC250_POINTER_MASKED_COLOR)

// enum dc_cursor_color_format, the value of CURSOR_CONTROL.CURSOR_MODE and CNVC_CUR0_CURSOR0_CONTROL.CUR0_MODE.
#define BC250_CURSOR_MODE_MONO 0u
#define BC250_CURSOR_MODE_COLOR_1BIT_AND 1u
#define BC250_CURSOR_MODE_PREMULTIPLIED 2u
#define BC250_CURSOR_MODE_STRAIGHT 3u

// The EnableHardwarePointer setting. Bit 0 opens the hardware pointer. Bit 1 scans Color pointers out as
// premultiplied alpha (the default is straight alpha). Bit 2 takes X and Y as the hot spot's position and subtracts
// XHot and YHot (the default takes them as the top left pixel, which is what d3dkmddi.h says). Bits 1 and 2 are the
// lab's A/B switches for the two contract points that no document on this disk settles.
#define BC250_CURSOR_SETTING_ON 0x1u
#define BC250_CURSOR_SETTING_PREMULTIPLIED 0x2u
#define BC250_CURSOR_SETTING_HOTSPOT_POSITION 0x4u
#define BC250_CURSOR_SETTING_MASK 0x7u

// What a pipe that the connector put on the screen gives the cursor (DcnCursorPipeReady): the clocks of
// hubp2_cursor_set_position's destination offset, the cursor width its DML plan budgeted, and for a pipe whose
// requestor the connector did not program (the inherited timing) the plan's two cursor TTU words
// (display_ttu_regs_st *_cur0).
typedef struct _BC250_CURSOR_PIPE {
    unsigned int RefClockKhz, PixelClockKhz, CursorWidth;
    int WriteTtu;
    unsigned int Delivery, DeliveryPre, QosLevelFixed, QosRampDisable;
} BC250_CURSOR_PIPE;

// Why a shape is refused. A log line carries the number, so this is a vocabulary: append, never renumber.
enum bc250_cursor_shape_verdict {
    BC250_CURSOR_SHAPE_OK = 0,
    BC250_CURSOR_SHAPE_ARGS = 1,            // no pixels, or no output
    BC250_CURSOR_SHAPE_FLAGS = 2,           // not exactly one of Monochrome, Color, MaskedColor, or a reserved bit
    BC250_CURSOR_SHAPE_SIZE = 3,            // a width or a height of 0 or above BC250_CURSOR_MAX_SIZE
    BC250_CURSOR_SHAPE_PITCH = 4,           // a pitch that does not hold one row
    BC250_CURSOR_SHAPE_BYTES = 5,           // the input is larger than BC250_CURSOR_RAW_MAX_BYTES
    BC250_CURSOR_SHAPE_ANGLE = 6,           // a rotation other than 0, 90, 180, 270
    BC250_CURSOR_SHAPE_VERDICTS
};

typedef struct _BC250_CURSOR_SHAPE {
    unsigned int Flags;                     // DXGK_POINTERFLAGS.Value
    unsigned int Width, Height, Pitch;      // as the DDI gives them: Height is one mask for Monochrome
    unsigned int XHot, YHot;
    const unsigned char* Pixels;
} BC250_CURSOR_SHAPE;

// What the cursor registers need of a rendered image.
typedef struct _BC250_CURSOR_IMAGE {
    unsigned int Width, Height;             // in the physical frame: swapped at 90 and 270 degrees
    unsigned int Mode;                      // BC250_CURSOR_MODE_*
    unsigned int XorPixels;                 // pixels that invert or XOR the screen (they need COLOR_1BIT_AND)
} BC250_CURSOR_IMAGE;

// The bytes of the input: two masks for Monochrome, one image otherwise. 0 for a refused shape.
static __inline unsigned int Bc250CursorShapeBytes(const BC250_CURSOR_SHAPE* Shape)
{
    unsigned long long bytes;
    if (!Shape) return 0u;
    bytes = (unsigned long long)Shape->Pitch * Shape->Height * (Shape->Flags == BC250_POINTER_MONOCHROME ? 2u : 1u);
    return bytes > BC250_CURSOR_RAW_MAX_BYTES ? 0u : (unsigned int)bytes;
}

static __inline unsigned int Bc250CursorShapeCheck(const BC250_CURSOR_SHAPE* Shape)
{
    unsigned int row;
    if (!Shape || !Shape->Pixels) return BC250_CURSOR_SHAPE_ARGS;
    if (Shape->Flags != BC250_POINTER_MONOCHROME && Shape->Flags != BC250_POINTER_COLOR &&
        Shape->Flags != BC250_POINTER_MASKED_COLOR) return BC250_CURSOR_SHAPE_FLAGS;
    if (!Shape->Width || !Shape->Height || Shape->Width > BC250_CURSOR_MAX_SIZE || Shape->Height > BC250_CURSOR_MAX_SIZE)
        return BC250_CURSOR_SHAPE_SIZE;
    row = Shape->Flags == BC250_POINTER_MONOCHROME ? (Shape->Width + 7u) / 8u : Shape->Width * 4u;
    if (Shape->Pitch < row) return BC250_CURSOR_SHAPE_PITCH;
    if (!Bc250CursorShapeBytes(Shape)) return BC250_CURSOR_SHAPE_BYTES;
    return BC250_CURSOR_SHAPE_OK;
}

static __inline int Bc250CursorAngleValid(unsigned int Angle)
{
    return Angle == 0u || Angle == 90u || Angle == 180u || Angle == 270u;
}

// The input pixel (I, J) that the physical image pixel (U, V) shows, for an image of Width x Height in the logical
// (desktop) orientation. The same counter-clockwise turn as Bc250RotationMapPixel: at 90 degrees the logical pixel
// (X + i, Y + j) lies at the physical (Y + j, PhysicalHeight - 1 - X - i).
static __inline void Bc250CursorSourcePixel(unsigned int Angle, unsigned int Width, unsigned int Height,
    unsigned int U, unsigned int V, unsigned int* I, unsigned int* J)
{
    switch (Angle) {
    case 90u:  *I = Width - 1u - V; *J = U;              break;
    case 180u: *I = Width - 1u - U; *J = Height - 1u - V; break;
    case 270u: *I = V;              *J = Height - 1u - U; break;
    default:   *I = U;              *J = V;              break;
    }
}

static __inline unsigned int Bc250CursorBit(const unsigned char* Row, unsigned int I)
{
    return (Row[I >> 3] >> (7u - (I & 7u))) & 1u;
}

// The 32-bit ARGB word of input pixel (I, J) before the mode choice. For Monochrome and MaskedColor the alpha byte
// is the AND bit as COLOR_1BIT_AND reads it (0xFF keeps the screen, 0 replaces it), the RGB bytes are XORed in.
static __inline unsigned int Bc250CursorInputWord(const BC250_CURSOR_SHAPE* Shape, unsigned int I, unsigned int J)
{
    const unsigned char* p = Shape->Pixels;
    if (Shape->Flags == BC250_POINTER_MONOCHROME) {
        const unsigned int andBit = Bc250CursorBit(p + (unsigned long long)J * Shape->Pitch, I);
        const unsigned int xorBit = Bc250CursorBit(p + ((unsigned long long)Shape->Height + J) * Shape->Pitch, I);
        return (andBit ? 0xFF000000u : 0u) | (xorBit ? 0x00FFFFFFu : 0u);
    }
    p += (unsigned long long)J * Shape->Pitch + 4ull * I;
    {
        const unsigned int word = (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
            ((unsigned int)p[3] << 24);
        // MaskedColor: the only mask values are 0 and 0xFF. Anything else is read by its high bit.
        if (Shape->Flags == BC250_POINTER_MASKED_COLOR) return (word & 0x00FFFFFFu) | ((word & 0x80000000u) ? 0xFF000000u : 0u);
        return word;
    }
}

// An AND-and-XOR word (Monochrome, MaskedColor) as a straight-alpha word, for an image with no XOR pixel: AND 0
// gives the opaque RGB value, AND 1 with RGB 0 gives a transparent pixel.
static __inline unsigned int Bc250CursorStraightOfMask(unsigned int Word)
{
    return (Word & 0xFF000000u) ? 0u : (0xFF000000u | (Word & 0x00FFFFFFu));
}

// Render Shape, turned by Angle, into Out: rows of BC250_CURSOR_PITCH_PIXELS words, top to bottom. Only the
// Image->Width x Image->Height rectangle at the origin is written. Color goes out unchanged in the alpha mode the
// setting chose. Monochrome and MaskedColor go out as straight alpha when no pixel inverts or XORs the screen (the
// common case, and the one that needs no AND-mask semantics of the hardware), else as COLOR_1BIT_AND.
static __inline unsigned int Bc250CursorRender(const BC250_CURSOR_SHAPE* Shape, unsigned int Angle,
    int Premultiplied, unsigned int* Out, BC250_CURSOR_IMAGE* Image)
{
    unsigned int verdict = Bc250CursorShapeCheck(Shape), u, v, i, j, word, xorPixels = 0;
    if (verdict != BC250_CURSOR_SHAPE_OK) return verdict;
    if (!Out || !Image) return BC250_CURSOR_SHAPE_ARGS;
    if (!Bc250CursorAngleValid(Angle)) return BC250_CURSOR_SHAPE_ANGLE;
    Image->Width = (Angle == 90u || Angle == 270u) ? Shape->Height : Shape->Width;
    Image->Height = (Angle == 90u || Angle == 270u) ? Shape->Width : Shape->Height;
    if (Shape->Flags != BC250_POINTER_COLOR) {
        for (j = 0; j < Shape->Height; j++)
            for (i = 0; i < Shape->Width; i++) {
                word = Bc250CursorInputWord(Shape, i, j);
                if ((word & 0xFF000000u) && (word & 0x00FFFFFFu)) xorPixels++;
            }
    }
    Image->XorPixels = xorPixels;
    Image->Mode = Shape->Flags == BC250_POINTER_COLOR ?
        (Premultiplied ? BC250_CURSOR_MODE_PREMULTIPLIED : BC250_CURSOR_MODE_STRAIGHT) :
        (xorPixels ? BC250_CURSOR_MODE_COLOR_1BIT_AND : BC250_CURSOR_MODE_STRAIGHT);
    for (v = 0; v < Image->Height; v++) {
        unsigned int* row = Out + (unsigned long long)v * BC250_CURSOR_PITCH_PIXELS;
        for (u = 0; u < Image->Width; u++) {
            Bc250CursorSourcePixel(Angle, Shape->Width, Shape->Height, u, v, &i, &j);
            word = Bc250CursorInputWord(Shape, i, j);
            if (Shape->Flags != BC250_POINTER_COLOR && !xorPixels) word = Bc250CursorStraightOfMask(word);
            row[u] = word;
        }
    }
    return BC250_CURSOR_SHAPE_OK;
}

// CURSOR_CONTROL.CURSOR_LINES_PER_CHUNK (hubp2_get_lines_per_chunk): enum cursor_lines_per_chunk, 2 to 16 lines as
// its log2 value (CURSOR_LINE_PER_CHUNK_2 = 1 ... CURSOR_LINE_PER_CHUNK_16 = 4). Monochrome takes 16 at any width.
static __inline unsigned int Bc250CursorLinesPerChunk(unsigned int Width, unsigned int Mode)
{
    if (Mode == BC250_CURSOR_MODE_MONO) return 4u;
    if (Width <= 32u) return 4u;
    if (Width <= 64u) return 3u;
    if (Width <= 128u) return 2u;
    return 1u;
}

// The plane the cursor is placed on, in the VidPN source's own (physical) pixels. A plane that shows the whole
// source is Map 0. A plane that is scaled into a rectangle of the source (a multiplane overlay plane) is Map 1:
// plane = (source - RecoutStart) * Viewport / Recout + ViewportStart, per axis. DstNumerator / DstDenominator is
// the horizontal destination-per-source ratio (recout over viewport) of hubp2_cursor_set_position's h_scale_ratio
// division.
typedef struct _BC250_CURSOR_PLANE {
    unsigned int SourceWidth, SourceHeight;     // the physical source mode
    unsigned int ViewportWidth, ViewportHeight; // the plane's own size (the source size for Map 0)
    int Map;
    int ViewportX, ViewportY, RecoutX, RecoutY;
    unsigned int RecoutWidth, RecoutHeight;
    unsigned int RefClockKhz, PixelClockKhz;    // DCHUB reference and the timing's pixel clock
    unsigned int DstNumerator, DstDenominator;
} BC250_CURSOR_PLANE;

// One placement: the five register fields of hubp2_cursor_set_position and the enable of both cursor blocks.
typedef struct _BC250_CURSOR_PLACE {
    unsigned int Enable;
    unsigned int X, Y, HotX, HotY, DstX;
    int Left, Top;                              // the top left pixel on the plane, before the clamp
} BC250_CURSOR_PLACE;

// The logical top left pixel of the image for a DDI position (X, Y): the position itself, or the position less the
// hot spot when the setting takes X and Y as the hot spot's position.
static __inline void Bc250CursorTopLeft(unsigned int Setting, int X, int Y, unsigned int XHot, unsigned int YHot,
    int* Left, int* Top)
{
    if (Setting & BC250_CURSOR_SETTING_HOTSPOT_POSITION) { X -= (int)XHot; Y -= (int)YHot; }
    *Left = X;
    *Top = Y;
}

// Floor division of a signed value by a positive one.
static __inline long long Bc250CursorFloorDiv(long long A, long long B)
{
    long long q = A / B;
    if ((A % B) != 0 && ((A < 0) != (B < 0))) q--;
    return q;
}

// Place an image of Image->Width x Image->Height whose logical top left pixel is (X, Y) (the DDI's numbers, less
// the hot spot when the setting says the DDI gives the hot spot) of a logical image Width x Height turned by Angle.
// Visible 0 or a placement wholly off the plane gives Enable 0 and leaves the other fields 0.
static __inline void Bc250CursorPlace(const BC250_CURSOR_PLANE* Plane, unsigned int Angle, unsigned int Width,
    unsigned int Height, const BC250_CURSOR_IMAGE* Image, int Visible, int X, int Y, BC250_CURSOR_PLACE* Out)
{
    long long left, top, pw, ph, dst, rx, ry;
    Out->Enable = 0; Out->X = Out->Y = Out->HotX = Out->HotY = Out->DstX = 0; Out->Left = Out->Top = 0;
    if (!Plane || !Image || !Visible || !Image->Width || !Image->Height || !Plane->SourceWidth ||
        !Plane->SourceHeight || !Plane->ViewportWidth || !Plane->ViewportHeight) return;
    pw = (long long)Plane->SourceWidth; ph = (long long)Plane->SourceHeight;
    // The logical rectangle (X, Y, Width, Height) in the physical frame (display_rotation.h's turn).
    switch (Angle) {
    case 90u:  left = Y;                 top = ph - X - (long long)Width;  break;
    case 180u: left = pw - X - (long long)Width; top = ph - Y - (long long)Height; break;
    case 270u: left = pw - Y - (long long)Height; top = X;                break;
    default:   left = X;                 top = Y;                          break;
    }
    if (Plane->Map) {
        if (!Plane->RecoutWidth || !Plane->RecoutHeight) return;
        left = Bc250CursorFloorDiv((left - Plane->RecoutX) * (long long)Plane->ViewportWidth,
                                   (long long)Plane->RecoutWidth) + Plane->ViewportX;
        top = Bc250CursorFloorDiv((top - Plane->RecoutY) * (long long)Plane->ViewportHeight,
                                  (long long)Plane->RecoutHeight) + Plane->ViewportY;
    }
    Out->Left = left < -0x7FFFFFFFll ? -0x7FFFFFFF : left > 0x7FFFFFFFll ? 0x7FFFFFFF : (int)left;
    Out->Top = top < -0x7FFFFFFFll ? -0x7FFFFFFF : top > 0x7FFFFFFFll ? 0x7FFFFFFF : (int)top;
    // CURSOR_POSITION is in the plane's surface (Left, Top above). hubp2_cursor_set_position: the edges are those of
    // the viewport, so the offsets that decide them start at the viewport's origin (0 for Map 0).
    rx = left - (Plane->Map ? Plane->ViewportX : 0);
    ry = top - (Plane->Map ? Plane->ViewportY : 0);
    if (rx >= (long long)Plane->ViewportWidth || ry >= (long long)Plane->ViewportHeight ||
        rx + (long long)Image->Width <= 0 || ry + (long long)Image->Height <= 0) return;
    // A negative surface position cannot go in the register: 0, and the part outside goes into the hot spot.
    // Visible, so a negative left is above -Image->Width (a viewport origin is never negative), and the hot spot
    // (8 bits, at most 255) holds it.
    if (left <= -(long long)Image->Width || top <= -(long long)Image->Height) return;
    Out->X = left < 0 ? 0u : (unsigned int)left;
    Out->Y = top < 0 ? 0u : (unsigned int)top;
    Out->HotX = left < 0 ? (unsigned int)-left : 0u;
    Out->HotY = top < 0 ? (unsigned int)-top : 0u;
    // dst_x_offset = max(src_x_offset, 0) * ref_clk / pixel_clk / h_scale_ratio, in whole reference clocks.
    dst = rx < 0 ? 0 : rx;
    if (Plane->RefClockKhz && Plane->PixelClockKhz) {
        dst = dst * (long long)Plane->RefClockKhz / (long long)Plane->PixelClockKhz;
        if (Plane->DstNumerator && Plane->DstDenominator)
            dst = dst * (long long)Plane->DstNumerator / (long long)Plane->DstDenominator;
    } else dst = 0;
    Out->DstX = dst > 0x1FFF ? 0x1FFFu : (unsigned int)dst;     // CURSOR_DST_X_OFFSET is 13 bits
    Out->Enable = 1;
}

#endif
