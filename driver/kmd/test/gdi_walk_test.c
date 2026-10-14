/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host test of the command-buffer walk (driver\kmd\gdi_walk.c), against an inaccessible page.
 *
 * Review r1 of 2026-10-14 found that the walk read the union member the opcode names before it had
 * checked CommandSize, so a command buffer that ends in the middle of its last command was read up
 * to 40 bytes past CommandLength. A test that only compares return values cannot see that: the
 * bytes behind the buffer are usually readable. So this test puts every command at the very end of
 * a committed page, with the next page inaccessible, and calls the production function. A read of
 * one byte past CommandLength is an access violation, and the case fails.
 *
 * The first case is the positive control: it reads the first byte of the second page itself and
 * fails if that does NOT raise an access violation. Without it a passing run would prove nothing.
 * The negative control was run by hand on 2026-10-14: with the decode moved back in front of
 * Bc250GdiCommandSize, six checks fail with an access violation (the four truncated lengths, the
 * ClearTypeBlend case and the walk to the end of a buffer), and none fails with it in its place.
 *
 * Run by driver\kmd\test\run_gdi_walk.ps1, which compiles the production gdi_walk.c as it is.
 */
#include <stdio.h>
#include <string.h>

#include "gdi_walk.h"

static unsigned long long g_checks;
static unsigned int g_failures;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* The command buffer: the last bytes of a committed page, with the page behind it inaccessible. */
static unsigned char* g_page;       /* first byte of the committed page */
static unsigned char* g_guard;      /* first byte of the inaccessible page */

static unsigned char* Place(unsigned int bytes)
{
    unsigned char* at = g_guard - bytes;
    memset(at, 0, bytes);
    return at;
}

/* One call of the production walk, with any access violation turned into a failed case. */
typedef struct _WALK_RESULT {
    int kind;
    unsigned int next;
    unsigned int why;
    unsigned int subCount;
    int faulted;
    BC250_GDI_OP op;
    const RECT* subRects;
} WALK_RESULT;

static WALK_RESULT Walk(const unsigned char* buffer, unsigned int bytes, unsigned int offset)
{
    WALK_RESULT r;

    memset(&r, 0, sizeof(r));
    __try {
        r.kind = Bc250GdiWalkCommand(buffer, bytes, offset, &r.op, &r.subRects, &r.subCount,
                                     &r.next, &r.why);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        r.faulted = 1;
    }
    return r;
}

/* The two fields of the header, written without the WDK structure, so that a case may build a
 * command buffer that is shorter than the structure. */
static void Head(unsigned char* at, unsigned int opCode, unsigned int commandSize)
{
    unsigned int head[2];
    head[0] = opCode;
    head[1] = commandSize;
    memcpy(at, head, sizeof(head));
}

static void PutPointer(unsigned char* at, const void* value)
{
    memcpy(at, &value, sizeof(value));
}

static void PutUint(unsigned char* at, unsigned int value)
{
    memcpy(at, &value, sizeof(value));
}

/* ---- the positive control ------------------------------------------------------------------ */

static void TestGuardPageIsArmed(void)
{
    volatile unsigned char seen = 0;
    int faulted = 0;

    __try {
        seen = g_guard[0];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = 1;
    }
    CHECK(faulted == 1);
    CHECK(seen == 0);

    /* and the last byte of the command buffer is readable, so a case that fails fails for the read
     * it is about and not because the whole page is gone */
    faulted = 0;
    __try {
        seen = g_guard[-1];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = 1;
    }
    CHECK(faulted == 0);
}

/* ---- a command buffer that ends inside its last command ------------------------------------- */

/* The defect of review r1, in the three lengths the producer can leave behind: the header alone,
 * a ColorFill cut in the middle of its sub-rectangle pointer, and four bytes short of its end.
 * Every one of them must stop the walk after the header and read nothing else. */
static void TestTruncatedCommand(void)
{
    static const unsigned int lengths[] = { 8u, 12u, 24u, 44u };
    unsigned int i;

    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        unsigned char* buffer = Place(lengths[i]);
        WALK_RESULT r;

        Head(buffer, DXGK_GDIOP_COLORFILL, 0x30u);
        r = Walk(buffer, lengths[i], 0u);
        CHECK(r.faulted == 0);
        CHECK(r.kind == BC250_GDI_CMD_STOP);
        CHECK(r.why == BC250_GDI_WHY_MALFORMED);
        CHECK(r.next == 0u);
    }

    /* The same for the largest fixed part, ClearTypeBlend at 0x50, one byte of which was the widest
     * over-read of the defect. */
    {
        unsigned char* buffer = Place(76u);
        WALK_RESULT r;

        Head(buffer, DXGK_GDIOP_CLEARTYPEBLEND, 0x50u);
        r = Walk(buffer, 76u, 0u);
        CHECK(r.faulted == 0);
        CHECK(r.kind == BC250_GDI_CMD_STOP);
    }
}

/* A header that does not fit at all: the walk must not read even the opcode. */
static void TestHeaderDoesNotFit(void)
{
    static const unsigned int lengths[] = { 0u, 4u };
    unsigned int i;

    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        unsigned char* buffer = Place(lengths[i]);
        WALK_RESULT r = Walk(buffer, lengths[i], 0u);

        CHECK(r.faulted == 0);
        CHECK(r.kind == BC250_GDI_CMD_STOP);
        CHECK(r.next == 0u);
    }

    /* and the same at the end of a longer buffer: Offset leaves four bytes */
    {
        unsigned char* buffer = Place(52u);
        WALK_RESULT r;

        Head(buffer, DXGK_GDIOP_COLORFILL, 0x30u);
        r = Walk(buffer, 52u, 48u);
        CHECK(r.faulted == 0);
        CHECK(r.kind == BC250_GDI_CMD_STOP);
    }
}

/* An opcode the table does not describe: stopped from the header, with no size and no member read. */
static void TestUnknownOpcode(void)
{
    static const unsigned int opCodes[] = { 0u, 8u, 9u, 0x80000000u, 0xFFFFFFFFu };
    unsigned int i;

    for (i = 0; i < sizeof(opCodes) / sizeof(opCodes[0]); i++) {
        unsigned char* buffer = Place(8u);
        WALK_RESULT r;

        Head(buffer, opCodes[i], 0x1000u);
        r = Walk(buffer, 8u, 0u);
        CHECK(r.faulted == 0);
        CHECK(r.kind == BC250_GDI_CMD_STOP);
        CHECK(r.why == BC250_GDI_WHY_OPCODE);
        CHECK(r.next == 0u);
    }
}

/* An escape is walkable on its header alone: the driver reads no member of it. */
static void TestEscapeOnHeader(void)
{
    unsigned char* buffer = Place(8u);
    WALK_RESULT r;

    Head(buffer, DXGK_GDIOP_ESCAPE, 8u);
    r = Walk(buffer, 8u, 0u);
    CHECK(r.faulted == 0);
    CHECK(r.kind == BC250_GDI_CMD_SKIP);
    CHECK(r.why == BC250_GDI_WHY_ESCAPE);
    CHECK(r.next == 8u);
}

/* A whole command whose last byte is the last byte of the page: the decode must read the member and
 * nothing behind it. ColorFill, with its sub-rectangle array inside its own fixed part. */
static void TestWholeCommandAtPageEnd(void)
{
    unsigned char* buffer = Place(0x30u);
    WALK_RESULT r;

    Head(buffer, DXGK_GDIOP_COLORFILL, 0x30u);
    PutUint(buffer + 0x1Cu, 1u);                /* NumSubRects */
    PutPointer(buffer + 0x20u, buffer + 8u);    /* pSubRects, at the DstRect of the command */
    PutUint(buffer + 0x08u, 10u);               /* DstRect.left */
    PutUint(buffer + 0x0Cu, 20u);               /* DstRect.top */
    PutUint(buffer + 0x10u, 30u);               /* DstRect.right */
    PutUint(buffer + 0x14u, 40u);               /* DstRect.bottom */
    r = Walk(buffer, 0x30u, 0u);
    CHECK(r.faulted == 0);
    CHECK(r.kind == BC250_GDI_CMD_DRAW);
    CHECK(r.why == BC250_GDI_WHY_DRAW);
    CHECK(r.next == 0x30u);
    CHECK(r.subCount == 1u);
    CHECK(r.subRects == (const RECT*)(buffer + 8u));
    CHECK(r.op.DstRect.Left == 10 && r.op.DstRect.Top == 20);
    CHECK(r.op.DstRect.Right == 30 && r.op.DstRect.Bottom == 40);

    /* Three rectangles do not fit between the header and the end of the command, so the array is
     * not inside it; the walk skips and reads none of it. */
    PutUint(buffer + 0x1Cu, 3u);
    r = Walk(buffer, 0x30u, 0u);
    CHECK(r.faulted == 0);
    CHECK(r.kind == BC250_GDI_CMD_SKIP);
    CHECK(r.why == BC250_GDI_WHY_OUTSIDE);
    CHECK(r.next == 0x30u);
}

/* The shape the diagnostic producer inside dxgkrnl builds for the Windows driver tests, when its own
 * gate is on: an 80-byte BitBlt whose pSubRects points at its own embedded DstRect, 0x18 bytes
 * into the command. The array overlaps the fixed part, which the containment floor admits because
 * the floor is the header. Three rectangles fit behind 0x18 in 80 bytes, four do not. */
static void TestEmbeddedSubRects(void)
{
    unsigned int count;

    for (count = 1u; count <= 4u; count++) {
        unsigned char* buffer = Place(80u);
        WALK_RESULT r;

        Head(buffer, DXGK_GDIOP_BITBLT, 80u);
        PutUint(buffer + 0x18u, 100u);                  /* DstRect.left, the first sub-rectangle */
        PutUint(buffer + 0x1Cu, 200u);
        PutUint(buffer + 0x20u, 300u);
        PutUint(buffer + 0x24u, 400u);
        PutUint(buffer + 0x30u, count);                 /* NumSubRects */
        PutPointer(buffer + 0x38u, buffer + 0x18u);     /* pSubRects */
        r = Walk(buffer, 80u, 0u);
        CHECK(r.faulted == 0);
        CHECK(r.next == 80u);
        if (count <= 3u) {
            CHECK(r.kind == BC250_GDI_CMD_DRAW);
            CHECK(r.why == BC250_GDI_WHY_DRAW);
            CHECK(r.subCount == count);
            CHECK(r.subRects == (const RECT*)(buffer + 0x18u));
            CHECK(r.op.DstRect.Left == 100 && r.op.DstRect.Bottom == 400);
        } else {
            CHECK(r.kind == BC250_GDI_CMD_SKIP);
            CHECK(r.why == BC250_GDI_WHY_OUTSIDE);
        }
    }
}

/* A pointer outside the command, in both directions, and a count no command can hold: skipped, and
 * no byte of the array read. The low one points at the command's own header, which is readable, so
 * a walk that trusted the pointer would not fault here; it would translate rectangles that are not
 * the command's. The high one points at the inaccessible page, which is where the producer's own
 * next command would start. */
static void TestSubRectsOutsideCommand(void)
{
    unsigned char* buffer = Place(0x30u);
    WALK_RESULT r;

    Head(buffer, DXGK_GDIOP_COLORFILL, 0x30u);
    PutUint(buffer + 0x1Cu, 1u);

    PutPointer(buffer + 0x20u, buffer);                 /* at the header: below the floor */
    r = Walk(buffer, 0x30u, 0u);
    CHECK(r.faulted == 0);
    CHECK(r.kind == BC250_GDI_CMD_SKIP);
    CHECK(r.why == BC250_GDI_WHY_OUTSIDE);

    PutPointer(buffer + 0x20u, g_guard);                /* the inaccessible page itself */
    r = Walk(buffer, 0x30u, 0u);
    CHECK(r.faulted == 0);
    CHECK(r.kind == BC250_GDI_CMD_SKIP);
    CHECK(r.why == BC250_GDI_WHY_OUTSIDE);

    PutPointer(buffer + 0x20u, buffer + 8u);
    PutUint(buffer + 0x1Cu, 0xFFFFFFFFu);               /* a count that cannot fit anywhere */
    r = Walk(buffer, 0x30u, 0u);
    CHECK(r.faulted == 0);
    CHECK(r.kind == BC250_GDI_CMD_SKIP);
    CHECK(r.why == BC250_GDI_WHY_OUTSIDE);

    PutUint(buffer + 0x1Cu, 0u);                        /* no array at all */
    PutPointer(buffer + 0x20u, NULL);
    r = Walk(buffer, 0x30u, 0u);
    CHECK(r.faulted == 0);
    CHECK(r.kind == BC250_GDI_CMD_SKIP);
    CHECK(r.why == BC250_GDI_WHY_EMPTY);
}

/* The whole walk, as both passes of DxgkDdiRenderGdi run it: a good command and then the remains of
 * one. The loop must draw the first and stop on the second, having read 56 bytes in all. */
static void TestWalkToTheEnd(void)
{
    unsigned char* buffer = Place(56u);
    unsigned int offset = 0;
    unsigned int drawn = 0;
    unsigned int stopped = 0;
    unsigned int guard;

    Head(buffer, DXGK_GDIOP_COLORFILL, 0x30u);
    PutUint(buffer + 0x1Cu, 2u);
    PutPointer(buffer + 0x20u, buffer + 8u);
    Head(buffer + 0x30u, DXGK_GDIOP_COLORFILL, 0x30u); /* 8 bytes of a 48-byte command */

    for (guard = 0; guard < 8u; guard++) {
        WALK_RESULT r = Walk(buffer, 56u, offset);

        CHECK(r.faulted == 0);
        if (r.kind == BC250_GDI_CMD_STOP) { stopped++; break; }
        if (r.kind == BC250_GDI_CMD_DRAW) { drawn++; }
        CHECK(r.next > offset);
        offset = r.next;
    }
    CHECK(drawn == 1u);
    CHECK(stopped == 1u);
    CHECK(offset == 0x30u);
}

int main(void)
{
    SYSTEM_INFO info;
    unsigned char* region;

    GetSystemInfo(&info);
    region = (unsigned char*)VirtualAlloc(NULL, (SIZE_T)info.dwPageSize * 2, MEM_RESERVE,
                                          PAGE_NOACCESS);
    if (!region) { printf("FAIL VirtualAlloc reserve %lu\n", GetLastError()); return 2; }
    g_page = (unsigned char*)VirtualAlloc(region, info.dwPageSize, MEM_COMMIT, PAGE_READWRITE);
    if (!g_page) { printf("FAIL VirtualAlloc commit %lu\n", GetLastError()); return 2; }
    g_guard = region + info.dwPageSize;

    TestGuardPageIsArmed();
    TestTruncatedCommand();
    TestHeaderDoesNotFit();
    TestUnknownOpcode();
    TestEscapeOnHeader();
    TestWholeCommandAtPageEnd();
    TestEmbeddedSubRects();
    TestSubRectsOutsideCommand();
    TestWalkToTheEnd();

    printf("gdi_walk_test: %llu checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
