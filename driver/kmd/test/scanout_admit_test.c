// Host test of the production scan-out admission rule: driver/kmd/scanout_admit.h is included, not
// copied, so the function this drives is the one the miniport links. M15.14, gate "scanout-admit".
//
// What it has to prove, in the order the risk runs:
//   1. The surface that reaches HUBP0 is refused unless every clause holds. One wrong address on this
//      part is not recoverable (facts M53), so each clause gets its own refusal and its own case.
//   2. dxgkrnl's own shared primary - the allocation DWM flips today - is admitted by exactly the four
//      checks it was admitted by before this header existed, with no segment, address or pitch
//      granularity requirement added to it. A regression there is a black screen on the lab.
//   3. A refusal never writes *Pitch or *Bytes, so a caller that ignores the status cannot reprogram
//      the plane with a refused layout.
#include <stdio.h>
#include <string.h>
#include "dcn_translate.h"
#include "scanout_admit.h"
#include "../../contract/bc250_scanout_caps.h"   /* M15.14: the caps flag that publishes the new rows */

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if(!(x)){++failures;printf("FAIL line %d: %s\n",__LINE__,#x);} } while(0)

#define POST_WIDTH 1920ul
#define POST_HEIGHT 1200ul
#define POST_PITCH 7680ul                       // DcnPrimaryPitch(1920): 1920 is already a multiple of 64
#define FLIP_SEGMENT 1ul                        // BC250_WDDM_SEGMENT_VRAM, the DirectFlip descriptor's
#define APERTURE_SEGMENT 2ul
#define D3DDDIFMT_A8R8G8B8 21ul
#define D3DDDIFMT_X8R8G8B8 22ul
#define D3DDDIFMT_A8B8G8R8 32ul
#define D3DDDIFMT_A2B10G10R10 31ul
#define D3DDDIFMT_A16B16G16R16F 113ul
#define D3DDDIFMT_A8 28ul

// The scan-out surface an application asks for: POST geometry, BGRA8, a page-aligned VRAM address.
static void Requested(BC250_SCANOUT_CANDIDATE* c)
{
    memset(c, 0, sizeof(*c));
    c->UmdAlloc = 1;
    c->ScanoutRequested = 1;
    c->Width = POST_WIDTH; c->Height = POST_HEIGHT; c->Pitch = POST_PITCH;
    c->Format = D3DDDIFMT_A8R8G8B8;
    c->Size = (unsigned long long)POST_PITCH * POST_HEIGHT;
    c->Segment = FLIP_SEGMENT;
    c->Address = 0x271000000ull;
}

// dxgkrnl's shared primary: an LB7A allocation that asked for nothing. PrimarySegment and the address
// are whatever the OS passed; neither was ever checked here and neither may start being checked.
static void Inherited(BC250_SCANOUT_CANDIDATE* c)
{
    Requested(c);
    c->UmdAlloc = 0;
    c->ScanoutRequested = 0;
    c->Segment = APERTURE_SEGMENT;
    c->Address = 0x271000123ull;                // not page aligned, and not in the flip segment
}

// The shape both shells actually produce: an LB7A type-0 surface whose E26R record carried the
// scan-out bit. UmdAlloc is 0 and ScanoutRequested is 1, and wddm.c takes the geometry from the LB7A
// description rather than from the BC2A scan-out words. The rule must treat it exactly as it treats a
// BC2A request, because it reaches the same plane.
static void Shell(BC250_SCANOUT_CANDIDATE* c)
{
    Requested(c);
    c->UmdAlloc = 0;
}

static int Admit(const BC250_SCANOUT_CANDIDATE* c, unsigned long* pitch, unsigned long long* bytes)
{
    return Bc250ScanoutAdmit(c, POST_WIDTH, POST_HEIGHT, FLIP_SEGMENT, pitch, bytes);
}

int main(void)
{
    BC250_SCANOUT_CANDIDATE c;
    unsigned long pitch;
    unsigned long long bytes;
    unsigned long i;

    // 1. The admitted scan-out surface, and what it publishes.
    Requested(&c); pitch = 0; bytes = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
    CHECK(pitch == POST_PITCH && bytes == (unsigned long long)POST_PITCH * POST_HEIGHT);

    // 2. No allocation at all, and the arguments a caller must supply.
    pitch = 0xdead; bytes = 0xbeef;
    CHECK(Bc250ScanoutAdmit(NULL, POST_WIDTH, POST_HEIGHT, FLIP_SEGMENT, &pitch, &bytes) ==
          BC250_SCANOUT_NO_ALLOCATION);
    CHECK(pitch == 0xdead && bytes == 0xbeef);
    Requested(&c);
    CHECK(Bc250ScanoutAdmit(&c, POST_WIDTH, POST_HEIGHT, FLIP_SEGMENT, NULL, &bytes) == BC250_SCANOUT_NO_ALLOCATION);
    CHECK(Bc250ScanoutAdmit(&c, POST_WIDTH, POST_HEIGHT, FLIP_SEGMENT, &pitch, NULL) == BC250_SCANOUT_NO_ALLOCATION);

    // 3. The old blanket refusal, narrowed to its honest meaning: an application allocation that never
    //    asked to be scanned out is still refused, whatever else is right about it.
    Requested(&c); c.ScanoutRequested = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_NOT_REQUESTED);

    // 4. Format. Only the table's SCANOUT_PRIMARY rows with a plane encoding of the same size (M15.14,
    //    0.7.216.20: B8G8R8A8, X8, R8G8B8A8, R10G10B10A2). The composed-only rows (A8, the FP16 HDR row) must
    //    never reach the plane, and nor must an unknown number. FP16 is refused at its own 8-byte pitch,
    //    so the refusal is the format clause and not a pitch that happens to be wrong.
    {
        static const unsigned long refused[] = {0ul, D3DDDIFMT_A8, 33ul, 35ul, D3DDDIFMT_A16B16G16R16F,
                                                0xffffffffull & 999ul};
        static const unsigned long admitted[] = {D3DDDIFMT_A8R8G8B8, D3DDDIFMT_X8R8G8B8, D3DDDIFMT_A8B8G8R8,
                                                 D3DDDIFMT_A2B10G10R10};
        for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            const AMDGPU_WDDM_SURFACE_FORMAT* row = amdgpu_wddm_surface_format_by_d3dddi((unsigned int)refused[i]);
            Requested(&c); c.Format = refused[i];
            if (row) { c.Pitch = POST_WIDTH * row->bytes_per_pixel; c.Size = (unsigned long long)c.Pitch * POST_HEIGHT; }
            CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_FORMAT);
        }
        for (i = 0; i < sizeof(admitted) / sizeof(admitted[0]); i++) {
            Requested(&c); c.Format = admitted[i]; pitch = 0; bytes = 0;
            CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
            CHECK(pitch == POST_PITCH && bytes == (unsigned long long)POST_PITCH * POST_HEIGHT);
            // Each one at a pitch that is not whole pixels, and one too narrow for the row.
            Requested(&c); c.Format = admitted[i]; c.Pitch = POST_PITCH + 2ul;
            c.Size = (unsigned long long)c.Pitch * POST_HEIGHT;
            CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_PITCH);
            Requested(&c); c.Format = admitted[i]; c.Pitch = POST_PITCH - 4ul;
            CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_PITCH);
        }
    }

    // 4b. The table and the plane encodings agree (M15.14). Every SCANOUT_PRIMARY row has a plane format
    //     whose bytes a pixel are the row's, and every encoding is reached from a SCANOUT_PRIMARY row: a
    //     row added to the table before the driver can program it fails here, not on the lab. The
    //     FIRMWARE_PLANE rows are exactly the ARGB8888 ones, the firmware's own format, and a shell may
    //     offer any other SCANOUT_PRIMARY row only with BC250_SCANOUT_CAPS_PLANE_FORMATS.
    {
        unsigned int count, r;
        unsigned long reached[BC250_PLANE_FORMATS] = {0};
        const AMDGPU_WDDM_SURFACE_FORMAT* rows = amdgpu_wddm_surface_formats(&count);
        for (r = 0; r < count; r++) {
            const unsigned long plane = Bc250PlaneFormatOf(rows[r].d3dddi);
            const int scanout = amdgpu_wddm_surface_admit(&rows[r], AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY) != 0;
            const int firmware = amdgpu_wddm_surface_admit(&rows[r], AMDGPU_WDDM_SURFACE_FIRMWARE_PLANE) != 0;
            if (scanout) {
                CHECK(Bc250PlaneEncoding(plane) != NULL);
                CHECK(Bc250PlaneEncoding(plane) && Bc250PlaneEncoding(plane)->BytesPerPixel == rows[r].bytes_per_pixel);
                CHECK(WddmSurfaceFormatBpp(rows[r].d3dddi, BC250_SURFACE_SCANOUT) == rows[r].bytes_per_pixel);
                if (plane < BC250_PLANE_FORMATS) reached[plane]++;
                CHECK(bc250_scanout_format_admitted(&rows[r], BC250_SCANOUT_CAPS_DIRECT_FLIP |
                                                              BC250_SCANOUT_CAPS_PLANE_FORMATS));
                CHECK(bc250_scanout_format_admitted(&rows[r], BC250_SCANOUT_CAPS_DIRECT_FLIP) == firmware);
            } else {
                CHECK(WddmSurfaceFormatBpp(rows[r].d3dddi, BC250_SURFACE_SCANOUT) == 0);
                CHECK(!bc250_scanout_format_admitted(&rows[r], BC250_SCANOUT_CAPS_DIRECT_FLIP |
                                                               BC250_SCANOUT_CAPS_PLANE_FORMATS));
            }
            CHECK(!firmware || (scanout && plane == BC250_PLANE_FORMAT_ARGB8888));
        }
        for (r = BC250_PLANE_FORMAT_ARGB8888; r < BC250_PLANE_FORMATS; r++) CHECK(reached[r] != 0);
        CHECK(!bc250_scanout_format_admitted(NULL, BC250_SCANOUT_CAPS_PLANE_FORMATS));
        // The encodings decode back to themselves, and the names cover every id.
        for (r = 1; r < BC250_PLANE_FORMATS; r++) {
            const BC250_PLANE_ENCODING* e = Bc250PlaneEncoding(r);
            CHECK(e && Bc250PlaneFormatDecode(e->HubpPixelFormat, e->CnvcPixelFormat, e->CrossbarCbB, e->CrossbarCrR) == r);
            CHECK(strcmp(Bc250PlaneFormatText(r), "none") != 0);
        }
        CHECK(Bc250PlaneEncoding(BC250_PLANE_FORMAT_NONE) == NULL && Bc250PlaneEncoding(BC250_PLANE_FORMATS) == NULL);
        CHECK(!strcmp(Bc250PlaneFormatText(BC250_PLANE_FORMATS), "unknown"));
        // AMD's values (hubp1_program_pixel_format, dpp201_cnv_setup): 8 and 10, the ABGR crossbar swap.
        CHECK(Bc250PlaneEncoding(BC250_PLANE_FORMAT_ARGB8888)->CrossbarCrR == 3 &&
              Bc250PlaneEncoding(BC250_PLANE_FORMAT_ARGB8888)->CrossbarCbB == 2);
        CHECK(Bc250PlaneEncoding(BC250_PLANE_FORMAT_ABGR8888)->CrossbarCrR == 2 &&
              Bc250PlaneEncoding(BC250_PLANE_FORMAT_ABGR8888)->HubpPixelFormat == 8);
        CHECK(Bc250PlaneEncoding(BC250_PLANE_FORMAT_ABGR2101010)->HubpPixelFormat == 10 &&
              Bc250PlaneEncoding(BC250_PLANE_FORMAT_ABGR2101010)->CnvcPixelFormat == 10);
        CHECK(Bc250PlaneFormatDecode(24, 25, 3, 2) == BC250_PLANE_FORMAT_NONE);   // FP16: no encoding yet
    }

    // 5. Geometry. One VidPN source mode exists, the POST one, so anything else is refused - including
    //    a 1080p surface, which is what a "fullscreen" game would ask for if the mode list grew.
    Requested(&c); c.Width = 1920ul; c.Height = 1080ul;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_GEOMETRY);
    Requested(&c); c.Width = POST_WIDTH - 1ul;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_GEOMETRY);
    Requested(&c); c.Height = POST_HEIGHT + 1ul;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_GEOMETRY);
    Requested(&c); c.Width = 0; c.Height = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_GEOMETRY);

    // 6. Pitch. No layout at all, a pitch too narrow for the row, and a pitch that is not a whole
    //    number of 4-byte pixels: HUBPREQ0_DCSURF_SURFACE_PITCH holds pitch/4 - 1, so an odd pitch
    //    would be programmed as a different stride than the application drew at. Refused for both
    //    kinds of candidate, because DcnSurfaceBytes is the clause that does it.
    Requested(&c); c.Pitch = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_PITCH);
    Requested(&c); c.Pitch = POST_WIDTH * 4ul - 4ul; c.Size = (unsigned long long)c.Pitch * POST_HEIGHT;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_PITCH);
    Requested(&c); c.Pitch = POST_PITCH + 2ul; c.Size = (unsigned long long)c.Pitch * POST_HEIGHT;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_PITCH);
    // A wider pitch than the rows need is legal and is what gets programmed.
    Requested(&c); c.Pitch = POST_PITCH + 256ul; c.Size = (unsigned long long)c.Pitch * POST_HEIGHT;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
    CHECK(pitch == POST_PITCH + 256ul && bytes == (unsigned long long)(POST_PITCH + 256ul) * POST_HEIGHT);

    // 7. Size. The rows must fit in the allocation, to the last byte.
    Requested(&c); c.Size -= 1ull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SIZE);
    Requested(&c); c.Size = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SIZE);

    // 8. Address alignment, for a scan-out request only.
    Requested(&c); c.Address = 0x271000001ull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ALIGNMENT);
    Requested(&c); c.Address = 0x271000fffull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ALIGNMENT);
    Requested(&c); c.Address = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);   // 0 is aligned; dcn.c decides the address

    // 9. Residency. The aperture is not scanned out, and neither is any segment but the one whose
    //    descriptor carries DirectFlip. A caller that cannot name that segment admits nothing.
    Requested(&c); c.Segment = APERTURE_SEGMENT;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SEGMENT);
    Requested(&c); c.Segment = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SEGMENT);
    Requested(&c); c.Segment = 3ul;              // the page-table segment
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SEGMENT);
    Requested(&c);
    CHECK(Bc250ScanoutAdmit(&c, POST_WIDTH, POST_HEIGHT, 0ul, &pitch, &bytes) == BC250_SCANOUT_SEGMENT);

    // 10. A refusal writes neither output. Checked on one clause of each half of the function.
    Requested(&c); c.Format = D3DDDIFMT_A16B16G16R16F; pitch = 0x11111111ul; bytes = 0x2222222222222222ull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_FORMAT);
    CHECK(pitch == 0x11111111ul && bytes == 0x2222222222222222ull);
    Requested(&c); c.Segment = APERTURE_SEGMENT;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SEGMENT);
    CHECK(pitch == 0x11111111ul && bytes == 0x2222222222222222ull);

    // 11. The inherited shared primary, which must behave exactly as it did before M15.14: admitted
    //     although its address is unaligned and its segment is not the flip one, because neither was
    //     ever part of its rule. This is the clause that keeps the lab's desktop on the screen.
    Inherited(&c); pitch = 0; bytes = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
    CHECK(pitch == POST_PITCH && bytes == (unsigned long long)POST_PITCH * POST_HEIGHT);
    Inherited(&c); c.Pitch = POST_PITCH + 2ul; c.Size = (unsigned long long)c.Pitch * POST_HEIGHT;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_PITCH);
    // and the four checks it always had still refuse it. From 0.7.216.20 its format clause is the same table
    // rule: an A8B8G8R8 shared primary (an R8G8B8A8 source mode) is a plane format now, FP16 is not.
    Inherited(&c); c.Format = D3DDDIFMT_A8B8G8R8;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
    Inherited(&c); c.Format = D3DDDIFMT_A16B16G16R16F;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_FORMAT);
    Inherited(&c); c.Height = POST_HEIGHT - 1ul;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_GEOMETRY);
    Inherited(&c); c.Size -= 1ull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SIZE);

    // 12. The POST mode is an argument, not a constant: the same candidate is admitted at its own
    //     geometry and refused at another's.
    Requested(&c);
    CHECK(Bc250ScanoutAdmit(&c, 1366ul, 768ul, FLIP_SEGMENT, &pitch, &bytes) == BC250_SCANOUT_GEOMETRY);

    // 12b. The shell's own shape, which is the only one either shell produces today: the request is
    //      what decides, not the provenance, so an LB7A surface that asked for scan-out gets every
    //      clause a BC2A one gets - the alignment and the segment included.
    Shell(&c); pitch = 0; bytes = 0;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
    CHECK(pitch == POST_PITCH && bytes == (unsigned long long)POST_PITCH * POST_HEIGHT);
    Shell(&c); c.Segment = APERTURE_SEGMENT;     // where an LB7A primary lands unless the bit moved it
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SEGMENT);
    Shell(&c); c.Address = 0x271000800ull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ALIGNMENT);
    Shell(&c); c.Format = D3DDDIFMT_A16B16G16R16F;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_FORMAT);
    Shell(&c); c.Format = D3DDDIFMT_A2B10G10R10;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
    Shell(&c); c.Height = POST_HEIGHT + 2ul;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_GEOMETRY);
    Shell(&c); c.Size -= 1ull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_SIZE);
    // The same four values with the request withdrawn are the inherited class again: admitted with no
    // alignment and no segment clause. The one bit is the whole difference.
    Shell(&c); c.ScanoutRequested = 0; c.Segment = APERTURE_SEGMENT; c.Address = 0x271000800ull;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);

    // 13. The status text covers every status and nothing outside the table.
    for (i = 0; i < BC250_SCANOUT_STATUSES; i++) CHECK(Bc250ScanoutStatusText((int)i)[0] != 0);
    CHECK(!strcmp(Bc250ScanoutStatusText(-1), "unknown"));
    CHECK(!strcmp(Bc250ScanoutStatusText(BC250_SCANOUT_STATUSES), "unknown"));
    CHECK(!strcmp(Bc250ScanoutStatusText(BC250_SCANOUT_ADMIT_OK), "ok"));
    CHECK(!strcmp(Bc250ScanoutStatusText(BC250_SCANOUT_SEGMENT), "segment"));

    // 14. The pitch the firmware itself left is the one a POST-geometry surface is expected to carry.
    CHECK(DcnPrimaryPitch(POST_WIDTH) == POST_PITCH);

    // 15. The create-time alignment and the flip-time clause are one number (0.7.209.1). A surface that
    //     asked for scan-out is created on the granularity its own clause demands, so the clause cannot
    //     refuse the transition flip of a correctly described surface - a refusal there blanks the output
    //     instead of falling back to composition. Everything else keeps the 64 bytes it had.
    CHECK(Bc250ScanoutCreateAlignment(1) == BC250_SCANOUT_ADDRESS_ALIGNMENT);
    CHECK(Bc250ScanoutCreateAlignment(1) == 4096ul);
    CHECK(Bc250ScanoutCreateAlignment(0) == 64ul);
    for (i = 0; i < 2; i++) {
        /* Every base the create alignment can produce is a base the clause admits, and the one below it
           is not: the two are the same rule read from both ends. */
        const unsigned long long base = 4ull * 1024ull * 1024ull * 1024ull;
        Shell(&c);
        c.Address = base + (unsigned long long)Bc250ScanoutCreateAlignment(1) * (i + 1u);
        CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
        Shell(&c);
        c.Address = base + (unsigned long long)Bc250ScanoutCreateAlignment(0) * (i + 1u);
        CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ALIGNMENT);
    }

    // 16. The BC2A create path (0.7.209.1). An application blob brings its own granularity, and the one
    //     thing it may not bring is less than the flip clause's page once it asked for scan-out.
    CHECK(Bc250ScanoutBlobAlignment(0, 0ull) == 4096ul);            // no request: the old default
    CHECK(Bc250ScanoutBlobAlignment(1, 0ull) == 4096ul);
    CHECK(Bc250ScanoutBlobAlignment(0, 64ull) == 64ul);             // honoured when nothing scans out
    CHECK(Bc250ScanoutBlobAlignment(1, 64ull) == 4096ul);           // the defect: 64 would be refused at flip
    CHECK(Bc250ScanoutBlobAlignment(1, 256ull) == 4096ul);
    CHECK(Bc250ScanoutBlobAlignment(1, 4096ull) == 4096ul);
    CHECK(Bc250ScanoutBlobAlignment(1, 65536ull) == 65536ul);       // a bigger ask is still honoured
    CHECK(Bc250ScanoutBlobAlignment(0, 65536ull) == 65536ul);
    CHECK(Bc250ScanoutBlobAlignment(1, 0x100000ull) == 0x100000ul); // the top of the honoured range
    CHECK(Bc250ScanoutBlobAlignment(0, 0x200000ull) == 4096ul);     // outside it: the default, not the ask
    CHECK(Bc250ScanoutBlobAlignment(0, 96ull) == 4096ul);           // not a power of two: the default
    CHECK(Bc250ScanoutBlobAlignment(0, 32ull) == 4096ul);           // below the range: the default
    CHECK(Bc250ScanoutBlobAlignment(1, 32ull) == 4096ul);
    for (i = 0; i < 2; i++) {
        /* Both create paths are the same number for the same intent, and no blob alignment the rule
           returns for a scan-out surface can land on a base the clause refuses. */
        const unsigned long long base = 4ull * 1024ull * 1024ull * 1024ull;
        const unsigned long long asked[] = { 0ull, 64ull, 96ull, 4096ull, 65536ull, 0x100000ull, 0x200000ull };
        unsigned n;
        CHECK(Bc250ScanoutBlobAlignment(1, (unsigned long long)Bc250ScanoutCreateAlignment(1)) ==
              Bc250ScanoutCreateAlignment(1));
        for (n = 0; n < sizeof(asked) / sizeof(asked[0]); n++) {
            const unsigned long align = Bc250ScanoutBlobAlignment(1, asked[n]);
            CHECK(align >= Bc250ScanoutCreateAlignment(1));
            Shell(&c);
            c.Address = base + (unsigned long long)align * (i + 1u);
            CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
        }
    }

    printf("scanout admission: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
