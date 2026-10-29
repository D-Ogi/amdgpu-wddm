/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// Host test of the FSR 4 identity policy (amd_identity_policy.c): the anti-cheat list match, the precedence and the
// 608-byte reply. run_amd_identity.ps1 builds it with the policy source and runs it.
#include <stdio.h>
#include <string.h>
#include "amd_identity_policy.h"
#include "../../contract/bc250_caps_query.h"

static int g_failures;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s (line %d)\n", what, __LINE__); g_failures++; } } while (0)

// An ASCII string as UTF-16 code units, for the name tests.
static unsigned int Wide(unsigned short* out, unsigned int size, const char* s)
{
    unsigned int n = 0;
    while (s[n] && n + 1 < size) { out[n] = (unsigned char)s[n]; n++; }
    out[n] = 0;
    return n;
}

static unsigned int Flags(const char* s)
{
    unsigned short w[300];
    unsigned int n = Wide(w, 300, s);
    return Bc250AmdIdentityNameFlags(w, n);
}

static unsigned int Get32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void TestNames(void)
{
    const unsigned int known = BC250_AMD_IDENTITY_NAME_KNOWN, listed = BC250_AMD_IDENTITY_NAME_KNOWN | BC250_AMD_IDENTITY_NAME_LISTED;
    unsigned short w[64];
    unsigned int i, n;

    CHECK(Flags("cs2.exe") == listed, "cs2.exe is listed");
    CHECK(Flags("CS2.EXE") == listed, "the match ignores case");
    CHECK(Flags("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe") == listed,
        "a full path matches on its file name");
    CHECK(Flags("D:/Games/PUBG/TslGame/Binaries/Win64/TslGame.exe") == listed, "a path with '/' matches on its file name");
    CHECK(Flags("cs2.exe.bak") == known, "cs2.exe.bak is not listed");
    CHECK(Flags("xcs2.exe") == known, "xcs2.exe is not listed");
    CHECK(Flags("cs2.ex") == known, "cs2.ex is not listed");
    CHECK(Flags("C:\\cs2.exe\\game.exe") == known, "a listed directory name does not count");
    CHECK(Flags("fsr4probe.exe") == known, "the test client is not listed");
    CHECK(Flags("witcher3.exe") == known, "witcher3.exe is not listed");
    CHECK(Flags("") == 0, "an empty name is not known");
    CHECK(Bc250AmdIdentityNameFlags(NULL, 10) == 0, "a NULL name is not known");
    n = Wide(w, 64, "cs2.exe");
    CHECK(Bc250AmdIdentityNameFlags(w, 0) == 0, "zero characters are not a name");
    CHECK(Bc250AmdIdentityNameFlags(w, 3) == known, "the length bounds the name: cs2 is not listed");
    CHECK(Bc250AmdIdentityNameFlags(w, 64) == listed, "a zero inside the length ends the name");
    w[3] = 0;
    CHECK(Bc250AmdIdentityNameFlags(w, n) == known, "a zero inside the name ends it");
    // A name without a terminating zero, the full length of the escape's array.
    for (i = 0; i < 64; i++) w[i] = 'a';
    CHECK(Bc250AmdIdentityNameFlags(w, 64) == known, "a name without a terminating zero is read to its length");
    // Every list entry matches itself, in upper case too.
    CHECK(Bc250AmdIdentityListCount() == 12, "the list has 12 entries");
    for (i = 0; i < Bc250AmdIdentityListCount(); i++) {
        char up[64];
        const char* e = Bc250AmdIdentityListEntry(i);
        unsigned int k;
        CHECK(e != NULL && Flags(e) == listed, "every entry matches itself");
        for (k = 0; e[k] && k + 1 < sizeof(up); k++) up[k] = (e[k] >= 'a' && e[k] <= 'z') ? (char)(e[k] - 32) : e[k];
        up[k] = 0;
        CHECK(Flags(up) == listed, "every entry matches in upper case");
    }
    CHECK(Bc250AmdIdentityListEntry(Bc250AmdIdentityListCount()) == NULL, "past the list is NULL");
}

static int Decide(int process, unsigned int names, unsigned int shell, unsigned int want_source)
{
    unsigned int source = 99;
    int on = Bc250AmdIdentityDecide(process, names, shell, &source);
    if (source != want_source) {
        printf("FAIL source %u, expected %u (process %d names 0x%X shell %u)\n", source, want_source, process, names, shell);
        g_failures++;
    }
    return on;
}

static void TestPrecedence(void)
{
    const unsigned int known = BC250_AMD_IDENTITY_NAME_KNOWN, listed = BC250_AMD_IDENTITY_NAME_KNOWN | BC250_AMD_IDENTITY_NAME_LISTED;
    unsigned int s;

    CHECK(Decide(1, known, BC250_AMD_IDENTITY_SHELL_NONE, BC250_AMD_IDENTITY_SOURCE_DEFAULT) == 1, "default is ON");
    CHECK(Decide(1, known, BC250_AMD_IDENTITY_SHELL_OFF, BC250_AMD_IDENTITY_SOURCE_SHELL) == 0, "shell 0 is OFF");
    CHECK(Decide(1, known, BC250_AMD_IDENTITY_SHELL_ON, BC250_AMD_IDENTITY_SOURCE_SHELL) == 1, "shell 1 is ON");
    for (s = 0; s < 3; s++) {
        CHECK(Decide(1, listed, s, BC250_AMD_IDENTITY_SOURCE_LIST) == 0, "a listed name is OFF whatever the shell sent");
        CHECK(Decide(0, known, s, BC250_AMD_IDENTITY_SOURCE_NO_PROCESS) == 0, "no process handle is OFF");
        CHECK(Decide(0, listed, s, BC250_AMD_IDENTITY_SOURCE_NO_PROCESS) == 0, "no process handle comes first");
    }
    CHECK(Decide(1, 0, BC250_AMD_IDENTITY_SHELL_NONE, BC250_AMD_IDENTITY_SOURCE_NO_NAME) == 0, "no name, no shell value is OFF");
    CHECK(Decide(1, 0, BC250_AMD_IDENTITY_SHELL_ON, BC250_AMD_IDENTITY_SOURCE_SHELL) == 1, "no name, shell 1 is ON");
    CHECK(Decide(1, 0, BC250_AMD_IDENTITY_SHELL_OFF, BC250_AMD_IDENTITY_SOURCE_SHELL) == 0, "no name, shell 0 is OFF");
    // An unknown shell word is not a value: the name decides.
    CHECK(Decide(1, known, 3, BC250_AMD_IDENTITY_SOURCE_DEFAULT) == 1, "shell word 3 is no value");
    CHECK(Bc250AmdIdentityDecide(1, known, 0, NULL) == 1, "a NULL source is allowed");
    // The union of dxgkrnl's name and the shell's name: listed by either is listed.
    CHECK(Decide(1, known | listed, BC250_AMD_IDENTITY_SHELL_ON, BC250_AMD_IDENTITY_SOURCE_LIST) == 0,
        "listed by the shell's name only is still OFF");
    for (s = 0; s < BC250_AMD_IDENTITY_SOURCE_COUNT; s++)
        CHECK(strcmp(Bc250AmdIdentitySourceName(s), "?") != 0, "every source has a name");
    CHECK(strcmp(Bc250AmdIdentitySourceName(BC250_AMD_IDENTITY_SOURCE_COUNT), "?") == 0, "past the sources is '?'");
}

static void TestFill(void)
{
    static const unsigned int sizes[] = {0, 4, 64, 600, 607, 609, 1024, 1471, 1472, 1520, 17248};
    unsigned char buf[17248 + 16];
    unsigned int i, k, nonzero;
    int on;

    for (on = 0; on <= 1; on++) {
        memset(buf, 0xA5, sizeof(buf));
        CHECK(Bc250AmdIdentityFill(buf, 608, on) == 1, "608 bytes are filled");
        CHECK(Get32(buf + 0x30) == 0x8Fu, "family 0x8F at 0x30");
        CHECK(Get32(buf + 0x34) == (on ? 0x32u : 0x84u), "revision at 0x34: 0x32 ON, 0x84 OFF");
        for (nonzero = 0, k = 0; k < 608; k++) if ((k < 0x30 || k >= 0x38) && buf[k] != 0) nonzero++;
        CHECK(nonzero == 0, "every other byte of the 608 is zero");
        for (k = 608; k < sizeof(buf); k++) if (buf[k] != 0xA5) break;
        CHECK(k == sizeof(buf), "nothing past 608 bytes is written");
    }
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        memset(buf, 0xA5, sizeof(buf));
        CHECK(Bc250AmdIdentityFill(buf, sizes[i], 1) == 0, "another size is refused");
        for (k = 0; k < sizeof(buf); k++) if (buf[k] != 0xA5) break;
        CHECK(k == sizeof(buf), "a refused size writes nothing");
        CHECK(Bc250AmdIdentityExtent(sizes[i]) == 0, "only 608 is the identity extent");
    }
    CHECK(Bc250AmdIdentityFill(NULL, 608, 1) == 0, "a NULL buffer is refused");
    CHECK(Bc250AmdIdentityExtent(608) == 1, "608 is the identity extent");
    // The query path admits 608 through the caps extent check before it reaches the identity answer, and our own
    // readers' sizes stay caps queries.
    CHECK(Bc250CapsQueryExtentSupported(608) == 1, "608 passes the caps extent check");
    CHECK(Bc250AmdIdentityExtent(1472) == 0 && Bc250AmdIdentityExtent(1520) == 0, "1472 and 1520 stay caps queries");
}

int main(void)
{
    TestNames();
    TestPrecedence();
    TestFill();
    printf("amd_identity: %s (%d failure%s)\n", g_failures ? "FAIL" : "PASS", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
