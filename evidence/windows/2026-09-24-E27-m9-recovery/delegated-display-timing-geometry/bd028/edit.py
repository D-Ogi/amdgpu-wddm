from pathlib import Path
r=Path('P:/bc-250/bc250-win')
p=r/'driver/kmd/test/paging_mc_test.c'; s=p.read_text(); s=s.replace('static int g_failures;', 'static int g_failures, g_checks;').replace('do { if (!(cond))', 'do { g_checks++; if (!(cond))')
s=s.replace('int main(void)\n{\n    const unsigned long long mcBase = 0xF400000000ull;\n    const unsigned long long vramBase = 0x270000000ull;\n    const unsigned long long vramLength = 0x200000000ull;   // 8 GB, the shape of M31, not its measurement', 'static void Geometry(unsigned long long mcBase, unsigned long long vramBase,\n                     unsigned long long vramLength)\n{')
a=s.index('    if (g_failures == 0)'); s=s[:a]+'''    // A whole page above the old 8 GiB ceiling must remain addressable.
    if (vramLength > (8ull << 30))
        RoundTrip(mcBase + (8ull << 30), mcBase, vramBase, vramLength, 4096);
}

int main(void)
{
    // Synthetic windows: varying both origins prevents the old base from becoming an assumption.
    Geometry(0xF400000000ull, 0x270000000ull, 8ull << 30);
    Geometry(0xE800000000ull, 0x670000000ull, 12ull << 30);
    Geometry(0xDC00000000ull, 0x1270000000ull, 16ull << 30);
    printf("paging_mc_test: 3 synthetic geometries, %d checks, %d failed\\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
'''; p.write_text(s)
p=r/'driver/kmd/test/dcn_translate_test.c'; s=p.read_text(); head=s.index('#include'); s='''// Host controls for the supplied DCN address geometry. All windows are synthetic,
// including the retained 256 MiB fixture. Larger fixtures prove arithmetic only,
// not firmware support, installed RAM, usable application capacity or residency.
'''+s[head:]; s=s.replace('static int g_failures;', 'static int g_failures, g_checks;').replace('do { if (!(cond))','do { g_checks++; if (!(cond))')
a=s.index('int main(void)'); b=s.index('    const unsigned long long surfaceBytes',a); s=s[:a]+'''static void Geometry(unsigned long long mcBase, unsigned long long vramBase,
                     unsigned long long vramLength)
{
'''+s[b:]; s=s.replace("// 1. Offset 0: the firmware's own address (M85's 0x270000000 <- MC 0xF400000000).", '// 1. Offset zero translates to the supplied CPU-visible origin.'); s=s.replace("// 2. An offset inside the carve-out (the shape of M94's fill target, MC + 0x1000000).", '// 2. An offset inside the supplied window.')
a=s.index('    if (g_failures == 0)'); s=s[:a]+'''    // Last complete pitched surface fits, including windows larger than 8 GiB.
    CHECK(DcnTranslateCardAddress(mcBase + vramLength - surfaceBytes,
                                 mcBase, vramBase, vramLength, &physical));
    CHECK(physical == vramBase + vramLength - surfaceBytes);
    CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));
    if (vramLength > (8ull << 30)) {
        CHECK(DcnTranslateCardAddress(mcBase + (8ull << 30), mcBase, vramBase, vramLength, &physical));
        CHECK(physical == vramBase + (8ull << 30));
        CHECK(DcnAddressFits(physical, vramBase, vramLength, surfaceBytes));
    }
}

int main(void)
{
    Geometry(0xF400000000ull, 0x270000000ull, 256ull << 20);
    Geometry(0xF400000000ull, 0x270000000ull, 8ull << 30);
    Geometry(0xE800000000ull, 0x670000000ull, 12ull << 30);
    Geometry(0xDC00000000ull, 0x1270000000ull, 16ull << 30);
    printf("dcn_translate_test: 4 synthetic geometries, %d checks, %d failed\\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
'''; p.write_text(s)
p=r/'tools/wddm_contract_check/host/qai_bridge.h'; s=p.read_text(); anchor='int  bc250h_start(int vram_enabled);'; s=s.replace(anchor,'''/* Explicit input only: this host harness cannot discover hardware geometry.
 * The 8 GiB default retains the old fixture; 12/16 GiB fixtures are synthetic. */
struct bc250h_geometry {
    unsigned long long vram_length, vram_physical, vram_mc_base;
    unsigned fb_width, fb_height, fb_pitch;
};
int bc250h_fixture_geometry(unsigned gib, struct bc250h_geometry* geometry);
int bc250h_start_geometry(int vram_enabled, const struct bc250h_geometry* geometry);
'''+anchor); p.write_text(s)
p=r/'tools/wddm_contract_check/host/qai_bridge.c'; s=p.read_text(); a=s.index(' * The adapter this runs against'); b=s.index(' */',a); s=s[:a]+''' * Adapter geometry is supplied explicitly. The default retains the historical 8 GiB host fixture;
 * larger fixtures are synthetic, not measurements or predictions of firmware layout. No discovery
 * or register access is performed by this initializer: Device->Mmio remains NULL.
'''+s[b:]; a=s.index('/* Unit A,'); b=s.index('    if (g_Started)',a)
s=s[:a]+'''/* Geometry belongs to the caller, not to a hidden assumption in the bridge. */
int bc250h_fixture_geometry(unsigned gib, struct bc250h_geometry* geometry)
{
    if (geometry == NULL || (gib != 8 && gib != 12 && gib != 16)) return 0;
    geometry->vram_length = (unsigned long long)gib << 30;
    geometry->vram_physical = gib == 8 ? 0x270000000ull :
                              gib == 12 ? 0x670000000ull : 0x1270000000ull;
    geometry->vram_mc_base = gib == 8 ? 0xF400000000ull :
                             gib == 12 ? 0xE800000000ull : 0xDC00000000ull;
    geometry->fb_width = 1920;
    geometry->fb_height = 1200;
    geometry->fb_pitch = 7680;
    return 1;
}

int bc250h_start(int vram_enabled)
{
    struct bc250h_geometry geometry;
    if (!bc250h_fixture_geometry(8, &geometry)) return 4;
    return bc250h_start_geometry(vram_enabled, &geometry);
}

int bc250h_start_geometry(int vram_enabled, const struct bc250h_geometry* geometry)
{
'''+s[b:]; s=s.replace('    if (g_Started) return 1;', '''    if (g_Started) return 1;
    if (geometry == NULL || !geometry->vram_length || !geometry->fb_width ||
        !geometry->fb_height || (geometry->fb_pitch & 3u) ||
        (ULONGLONG)geometry->fb_width * 4 > geometry->fb_pitch ||
        (ULONGLONG)geometry->fb_pitch * geometry->fb_height > geometry->vram_length ||
        geometry->vram_physical > MAXULONGLONG - geometry->vram_length ||
        geometry->vram_mc_base > MAXULONGLONG - geometry->vram_length) return 4;''',1)
for old,new in {'BC250H_FB_WIDTH':'geometry->fb_width','BC250H_FB_HEIGHT':'geometry->fb_height','BC250H_FB_PITCH':'geometry->fb_pitch','BC250H_VRAM_PHYSICAL':'geometry->vram_physical','BC250H_VRAM_LENGTH':'geometry->vram_length','BC250H_VRAM_MC_BASE':'geometry->vram_mc_base'}.items(): s=s.replace(old,new)
# Minimal signature drift only; these remain test stubs, not memory implementation.
s=s.replace('void VidMmStart(_In_', 'NTSTATUS VidMmStart(_In_').replace('    g_Shim.vidmm_start++;\n}', '    g_Shim.vidmm_start++;\n    return STATUS_SUCCESS;\n}')
s=s.replace('void VidMmUpdatePageTable(_In_', 'NTSTATUS VidMmUpdatePageTable(_In_').replace('    g_Shim.vidmm_update_page_table++;\n}', '    g_Shim.vidmm_update_page_table++;\n    return STATUS_SUCCESS;\n}')
p.write_text(s)
p=r/'tools/wddm_contract_check/host/qai_test.c'; s=p.read_text(); a=s.index('int main(int argc, char** argv)'); tail=s[a:]; tail=tail.replace('    int rc;','    int rc;\n    unsigned fixture_gib = 8;\n    struct bc250h_geometry geometry;',1); tail=tail.replace('''    for (i = 1; i < (unsigned)argc; i++)
        if (strcmp(argv[i], "-v") == 0) g_Verbose = 1;''','''    for (i = 1; i < (unsigned)argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_Verbose = 1;
        else if (strcmp(argv[i], "--vram-gib") == 0 && i + 1 < (unsigned)argc) {
            const char* size = argv[++i];
            if (strcmp(size, "8") == 0) fixture_gib = 8;
            else if (strcmp(size, "12") == 0) fixture_gib = 12;
            else if (strcmp(size, "16") == 0) fixture_gib = 16;
            else { fprintf(stderr, "--vram-gib requires 8, 12 or 16 (synthetic fixture)\\n"); return 2; }
        } else { fprintf(stderr, "usage: qai_test [-v] [--vram-gib 8|12|16]\\n"); return 2; }
    }
    if (!bc250h_fixture_geometry(fixture_gib, &geometry)) return 2;
    printf("Synthetic geometry: %u GiB, CPU base 0x%llX, MC base 0x%llX; no hardware discovery\\n",
           fixture_gib, geometry.vram_physical, geometry.vram_mc_base);'''); tail=tail.replace('bc250h_start(1)','bc250h_start_geometry(1, &geometry)').replace('rc = bc250h_start(0);','rc = bc250h_start_geometry(0, &geometry);'); s=s[:a]+tail; p.write_text(s)
