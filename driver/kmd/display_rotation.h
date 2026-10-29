#ifndef BC250_DISPLAY_ROTATION_H
#define BC250_DISPLAY_ROTATION_H

// BD-247: display rotation (portrait, landscape flipped, portrait flipped). docs/design/display-rotation.md.
//
// The route. The scan-out pipe does not rotate. The compositor does: the shared primary of a rotated path keeps
// the physical (unrotated) size of the source mode, and DWM, which is rotation-aware, draws its content already
// rotated into it ("The Microsoft DirectX graphics kernel subsystem uses non-rotated surface dimensions to create
// the shared primary surface", display/supporting-rotation-in-a-display-miniport-driver.md; smooth rotation needs
// two modes of the same width, height and stride that differ only in orientation,
// display/optimized-screen-rotation-support.md). A flip carries no rotation, so the plane scans out what it is
// given, as it does at identity. HUBP rotation would be the other route, and DCN 2.0 refuses it for the surfaces
// this driver scans out: a linear surface cannot have a vertical source scan (Linux v6.18
// display_mode_vba_20v2.c, SourceFormatPixelAndScanSupport), and 90 and 270 degrees are a vertical scan.
//
// What the driver must still do itself:
//   - offer Rotate90, Rotate180 and Rotate270 with Offset0 on the path, and accept them at commit and at
//     DxgkDdiUpdateActiveVidPnPresentPath (smooth rotation);
//   - rotate the bugcheck screen, which the OS draws in the logical orientation (DxgkDdiSystemDisplayWrite);
//   - refuse what it cannot do: a Present with DXGK_PRESENTFLAGS.Rotate (the copy engine path writes rows, it
//     cannot transpose them), the path-independent offsets (no clone), and DirectFlip of an application surface
//     while the path is rotated (the scan-out caps trailer closes it, so DWM composes and rotates that chain).
//
// Plain integers and no WDK header, like scanout_admit.h, so the host test (test/display_rotation_test.c, gate
// "display-rotation") compiles this text and not a copy of it. display.c holds the values against d3dkmdt.h.

// D3DKMDT_VIDPN_PRESENT_PATH_ROTATION (d3dkmdt.h, WDK 10.0.26100). The offset forms run from 5 to 16.
#define BC250_VPPR_UNINITIALIZED 0u
#define BC250_VPPR_IDENTITY 1u
#define BC250_VPPR_ROTATE90 2u
#define BC250_VPPR_ROTATE180 3u
#define BC250_VPPR_ROTATE270 4u
#define BC250_VPPR_IDENTITY_OFFSET90 5u
#define BC250_VPPR_ROTATE270_OFFSET270 16u
#define BC250_VPPR_UNPINNED 254u
#define BC250_VPPR_NOTSPECIFIED 255u

// The answer for a value that this driver cannot show.
#define BC250_ROTATION_REFUSED 0xFFFFFFFFu

// The angle, counter-clockwise as GDI counts it, of a path rotation value, or BC250_ROTATION_REFUSED.
// A value that pins no rotation (uninitialized, unpinned, not specified) is the identity: nothing is rotated
// until dxgkrnl pins an angle, and a commit that pins none shows the content as it is.
// FullWddm 0 is the display-only table: its present copies rows unrotated (Bc250PresentDisplayOnly), so it
// keeps the identity alone, as before BD-247.
static __inline unsigned int Bc250RotationAngle(unsigned int Rotation, int FullWddm)
{
    switch (Rotation) {
    case BC250_VPPR_UNINITIALIZED:
    case BC250_VPPR_IDENTITY:
    case BC250_VPPR_UNPINNED:
    case BC250_VPPR_NOTSPECIFIED:
        return 0u;
    case BC250_VPPR_ROTATE90: return FullWddm ? 90u : BC250_ROTATION_REFUSED;
    case BC250_VPPR_ROTATE180: return FullWddm ? 180u : BC250_ROTATION_REFUSED;
    case BC250_VPPR_ROTATE270: return FullWddm ? 270u : BC250_ROTATION_REFUSED;
    default:
        // The offsets (5 to 16) say that one source feeds paths of different orientations (clone). This
        // adapter has one path, so the primary path's support carries Offset0 alone, and dxgkrnl bugchecks a
        // test-signed system whose primary path offers any other offset (E16 run 007).
        return BC250_ROTATION_REFUSED;
    }
}

// The path rotation value of an admitted angle, the one the driver keeps in BC250_DEVICE.Rotation.
static __inline unsigned int Bc250RotationValue(unsigned int Angle)
{
    return Angle == 90u ? BC250_VPPR_ROTATE90 : Angle == 180u ? BC250_VPPR_ROTATE180 :
        Angle == 270u ? BC250_VPPR_ROTATE270 : BC250_VPPR_IDENTITY;
}

// The rotation support of the one path, written into a D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT (or the host
// test's copy of it): a macro, because that type is a WDK bit field and this header names no WDK type.
// dxgkrnl reads the support on every pfnUpdatePathSupportInfo, and a full miniport must state Offset0 there
// (display.c, E16 run 007), so the full table always writes Identity and Offset0. The display-only table keeps
// what it had: Identity, and dxgkrnl sets the offset itself.
#define BC250_ROTATION_SUPPORT_FILL(Support, FullWddm)                                                          \
    do {                                                                                                     \
        (Support).Identity = 1;                                                                              \
        (Support).Rotate90 = (FullWddm) ? 1 : 0;                                                             \
        (Support).Rotate180 = (FullWddm) ? 1 : 0;                                                            \
        (Support).Rotate270 = (FullWddm) ? 1 : 0;                                                            \
        (Support).Offset0 = (FullWddm) ? 1 : 0;                                                              \
        (Support).Offset90 = 0;                                                                              \
        (Support).Offset180 = 0;                                                                             \
        (Support).Offset270 = 0;                                                                             \
    } while (0)

// The size the bugcheck screen is drawn at: the logical one, which swaps the physical width and height at 90
// and 270 degrees (DxgkDdiSystemDisplayEnable returns it, and the OS draws in it).
static __inline void Bc250RotationLogicalSize(unsigned int Angle, unsigned int PhysicalWidth,
    unsigned int PhysicalHeight, unsigned int* Width, unsigned int* Height)
{
    const int swap = Angle == 90u || Angle == 270u;
    *Width = swap ? PhysicalHeight : PhysicalWidth;
    *Height = swap ? PhysicalWidth : PhysicalHeight;
}

// The physical pixel that shows logical pixel (X, Y) of a source rotated by Angle. The content turns counter-
// clockwise on the panel: at 90 degrees the logical top edge lies along the physical left edge, and the logical
// left edge along the physical bottom edge. (X, Y) must lie inside the logical size; the result then lies
// inside the physical one.
static __inline void Bc250RotationMapPixel(unsigned int Angle, unsigned int PhysicalWidth,
    unsigned int PhysicalHeight, unsigned int X, unsigned int Y, unsigned int* Px, unsigned int* Py)
{
    switch (Angle) {
    case 90u:  *Px = Y;                      *Py = PhysicalHeight - 1u - X; break;
    case 180u: *Px = PhysicalWidth - 1u - X; *Py = PhysicalHeight - 1u - Y; break;
    case 270u: *Px = PhysicalWidth - 1u - Y; *Py = X;                      break;
    default:   *Px = X;                      *Py = Y;                      break;
    }
}

// DxgkDdiSystemDisplayWrite with a rotation: copy a block of 32-bit pixels at logical position (X, Y) into the
// physical frame. The block is clipped to the logical size and to the source stride. No pool, no lock and no
// call: this runs at the bugcheck's IRQL. At angle 0 each row is one block copy, as the copy before BD-247 was.
static __inline void Bc250RotationWriteBlock(unsigned int Angle, unsigned char* Frame, unsigned int Pitch,
    unsigned int PhysicalWidth, unsigned int PhysicalHeight, const unsigned char* Source, unsigned int SourceWidth,
    unsigned int SourceHeight, unsigned int SourceStride, unsigned int X, unsigned int Y)
{
    unsigned int logicalWidth, logicalHeight, width, height, row, column, px, py;
    if (!Frame || !Source || !PhysicalWidth || !PhysicalHeight) return;
    Bc250RotationLogicalSize(Angle, PhysicalWidth, PhysicalHeight, &logicalWidth, &logicalHeight);
    if (X >= logicalWidth || Y >= logicalHeight) return;
    width = SourceWidth < logicalWidth - X ? SourceWidth : logicalWidth - X;
    if (width > SourceStride / 4u) width = SourceStride / 4u;
    height = SourceHeight < logicalHeight - Y ? SourceHeight : logicalHeight - Y;
    for (row = 0; row < height; row++) {
        const unsigned char* from = Source + (unsigned long long)row * SourceStride;
        if (Angle != 90u && Angle != 180u && Angle != 270u) {
            unsigned char* to = Frame + (unsigned long long)(Y + row) * Pitch + 4ull * X;
            for (column = 0; column < width * 4u; column++) to[column] = from[column];
            continue;
        }
        for (column = 0; column < width; column++) {
            unsigned char* to;
            Bc250RotationMapPixel(Angle, PhysicalWidth, PhysicalHeight, X + column, Y + row, &px, &py);
            to = Frame + (unsigned long long)py * Pitch + 4ull * px;
            to[0] = from[4u * column]; to[1] = from[4u * column + 1u];
            to[2] = from[4u * column + 2u]; to[3] = from[4u * column + 3u];
        }
    }
}

#endif
