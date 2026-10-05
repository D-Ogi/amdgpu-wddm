from pathlib import Path
root=Path('P:/bc-250'); out=root/'scratch/m9/bd028'
s=(root/'bc250-win/tools/wddm_contract_check/host/qai_bridge.c').read_text()
s=s[:s.index('/* ---- the table ')]
s+='''
/* Test boundary: captures initializer input, does not emulate production WddmStart. */
BOOLEAN WddmGateOpen(void) { return TRUE; }
void WddmBuildTable(DRIVER_INITIALIZATION_DATA* table) { RtlZeroMemory(table,sizeof(*table)); }
NTSTATUS WddmStart(BC250_DEVICE* device) { device->Wddm=(PVOID)1;return STATUS_SUCCESS; }
void WddmStop(BC250_DEVICE* device) { device->Wddm=NULL; }
void bc250h_shim_defaults(struct bc250h_gfx_control* out) { RtlZeroMemory(out,sizeof(*out)); }
void bc250h_shim_set(const struct bc250h_gfx_control* control) { UNREFERENCED_PARAMETER(control); }
void bc250h_shim_reset_counters(void) { }
static unsigned checks,failures;
#define REQUIRE(x) do { ++checks;if(!(x))++failures; } while(0)
static void CheckGeometry(const struct bc250h_geometry* g,int enabled)
{
    REQUIRE(bc250h_start_geometry(enabled,g)==0);
    REQUIRE(g_Device.VramEnabled==(BOOLEAN)enabled);
    REQUIRE(g_Device.VramLength==(enabled?g->vram_length:0));
    REQUIRE((ULONGLONG)g_Device.VramPhysical.QuadPart==(enabled?g->vram_physical:0));
    REQUIRE(g_Device.VramMcBase==(enabled?g->vram_mc_base:0));
    REQUIRE((ULONGLONG)g_Device.Post.PhysicAddress.QuadPart==g->vram_physical);
    REQUIRE(g_Device.Post.Width==g->fb_width);
    REQUIRE(g_Device.Post.Height==g->fb_height);
    REQUIRE(g_Device.Post.Pitch==g->fb_pitch);
    REQUIRE(g_Device.Mmio==NULL);
    bc250h_stop();
    REQUIRE(!g_Started);
}
unsigned geometry_controls(unsigned* count)
{
    struct bc250h_geometry g;
    unsigned gib;
    for(gib=8;gib<=16;gib+=4) {
        REQUIRE(bc250h_fixture_geometry(gib,&g));
        REQUIRE(g.vram_length==((ULONGLONG)gib<<30));
        CheckGeometry(&g,1);
        CheckGeometry(&g,0);
    }
    REQUIRE(bc250h_start(1)==0);
    REQUIRE(g_Device.VramLength==(8ull<<30));
    REQUIRE(g_Device.VramPhysical.QuadPart==0x270000000ll);
    REQUIRE(g_Device.VramMcBase==0xF400000000ull);
    bc250h_stop();
    /* A caller can supply geometry independently of the named fixtures. */
    g.vram_length=10ull<<30;g.vram_physical=0x3450000000ull;g.vram_mc_base=0xA000000000ull;
    g.fb_width=1680;g.fb_height=1050;g.fb_pitch=6912;
    CheckGeometry(&g,1);
    *count=checks;return failures;
}
'''
(out/'geometry_actual.c').write_text(s)
(out/'geometry_main.c').write_text('''#include <stdio.h>
unsigned geometry_controls(unsigned* count);
int main(void) { unsigned count=0,fail=geometry_controls(&count);printf("qai geometry handoff: %u checks, %u failed\\n",count,fail);return fail?1:0; }
''')
