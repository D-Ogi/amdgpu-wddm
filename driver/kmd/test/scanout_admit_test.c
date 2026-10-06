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

    // 4. Format. Only the table's SCANOUT_PRIMARY rows; the composed-only rows that carry the HDR and
    //    10-bit work must never reach the plane, and nor must an unknown number.
    {
        static const unsigned long refused[] = {0ul, 28ul, D3DDDIFMT_A2B10G10R10, D3DDDIFMT_A8B8G8R8, 33ul, 35ul,
                                                D3DDDIFMT_A16B16G16R16F, 0xffffffffull & 999ul};
        for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            Requested(&c); c.Format = refused[i];
            CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_FORMAT);
        }
        Requested(&c); c.Format = D3DDDIFMT_X8R8G8B8;
        CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_ADMIT_OK);
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
    Requested(&c); c.Format = D3DDDIFMT_A2B10G10R10; pitch = 0x11111111ul; bytes = 0x2222222222222222ull;
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
    // and the four checks it always had still refuse it.
    Inherited(&c); c.Format = D3DDDIFMT_A8B8G8R8;
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
    Shell(&c); c.Format = D3DDDIFMT_A2B10G10R10;
    CHECK(Admit(&c, &pitch, &bytes) == BC250_SCANOUT_FORMAT);
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

    printf("scanout admission: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
