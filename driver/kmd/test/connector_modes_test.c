// Actual display.c selection helpers, with portable DDI value fixtures.
#include <stdio.h>
#include <string.h>
#include <stddef.h>
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef unsigned long long ULONGLONG;
typedef size_t SIZE_T;
typedef unsigned char BOOLEAN;
typedef unsigned int D3DKMDT_VIDPN_PRESENT_PATH_SCALING;
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define D3DKMDT_VSS_OTHER 255u
#define D3DDDI_VSSLO_PROGRESSIVE 1u
#define D3DKMDT_VPPR_UNINITIALIZED 0u
#define D3DKMDT_VPPR_IDENTITY 1u
#define D3DKMDT_VPPR_UNPINNED 254u
#define D3DKMDT_VPPS_UNINITIALIZED 0u
#define D3DKMDT_VPPS_IDENTITY 1u
#define D3DKMDT_VPPS_UNPINNED 254u
#define D3DKMDT_VPPS_NOTSPECIFIED 255u
#define D3DKMDT_RMT_GRAPHICS 1u
#define BC250_CHILD_UID 0x250001u
#define BC250_SINK_NONE 0u
typedef struct { UINT cx,cy; } REGION;
typedef struct { UINT Numerator,Denominator; } RATIONAL;
typedef struct {
    UINT VideoStandard;
    REGION TotalSize,ActiveSize;
    RATIONAL VSyncFreq,HSyncFreq;
    SIZE_T PixelRate;
    UINT ScanLineOrdering;
} D3DKMDT_VIDEO_SIGNAL_INFO;
typedef struct {
    UINT VidPnSourceId,VidPnTargetId;
    struct { UINT Rotation,Scaling; } ContentTransformation;
    REGION VisibleFromActiveTLOffset,VisibleFromActiveBROffset;
} D3DKMDT_VIDPN_PRESENT_PATH;
typedef struct {
    UINT Type;
    struct { struct { REGION PrimSurfSize,VisibleRegionSize; ULONG PixelFormat,Stride; } Graphics; } Format;
} D3DKMDT_VIDPN_SOURCE_MODE;
typedef struct { int ComposedSourceModes; } BC250_DEVICE;
#include "edid.h"
#include "display_modes.h"
#include "dcn_translate.h"
typedef struct {
    struct { ULONG Kind,TargetCount; BC250_TARGET_TIMING Targets[BC250_EDID_MAX_MODES]; } Sink;
} BC250_CONNECTOR_SNAPSHOT;
#include "connector_modes_actual.inc"
static unsigned checks,failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL CHECK line %u: %s\n",__LINE__,#x); } } while (0)
int main(void)
{
    BC250_DEVICE device = {0};
    BC250_CONNECTOR_SNAPSHOT snapshot = {0};
    BC250_TARGET_TIMING *t = &snapshot.Sink.Targets[0];
    D3DKMDT_VIDEO_SIGNAL_INFO signal,bad;
    D3DKMDT_VIDPN_SOURCE_MODE source = {0};
    D3DKMDT_VIDPN_PRESENT_PATH path = {0};
    ULONG formats[BC250_SOURCE_MODE_MAX];
    snapshot.Sink.Kind = 1; snapshot.Sink.TargetCount = 2;
    t->PixelClockKhz = 148500; t->HActive = 1920; t->HBlank = 280;
    t->VActive = 1080; t->VBlank = 45;
    snapshot.Sink.Targets[1] = *t;
    snapshot.Sink.Targets[1].PixelClockKhz = 74250;
    ConnectorSignal(t,&signal);
    CHECK(signal.PixelRate == 148500000 && signal.TotalSize.cx == 2200 && signal.TotalSize.cy == 1125);
    CHECK(ConnectorSignalMatches(&signal,t));
    bad = signal; bad.VSyncFreq.Numerator = 60; bad.VSyncFreq.Denominator = 1;
    CHECK(ConnectorSignalMatches(&bad,t));
    bad.PixelRate++; CHECK(!ConnectorSignalMatches(&bad,t));
    bad = signal; bad.TotalSize.cx++; CHECK(!ConnectorSignalMatches(&bad,t));
    bad = signal; bad.HSyncFreq.Denominator = 0; CHECK(!ConnectorSignalMatches(&bad,t));
    bad = signal; bad.ScanLineOrdering = 2; CHECK(!ConnectorSignalMatches(&bad,t));
    bad = signal; bad.VSyncFreq.Numerator++; CHECK(!ConnectorSignalMatches(&bad,t));
    CHECK(Bc250SourceModeFormats(0,formats,BC250_SOURCE_MODE_MAX) != 0);
    source.Type = D3DKMDT_RMT_GRAPHICS;
    source.Format.Graphics.PrimSurfSize = signal.ActiveSize;
    source.Format.Graphics.VisibleRegionSize = signal.ActiveSize;
    source.Format.Graphics.PixelFormat = formats[0];
    source.Format.Graphics.Stride = 7680;
    CHECK(ConnectorSourceMatches(&device,&source,t));
    source.Format.Graphics.PrimSurfSize.cx--; CHECK(!ConnectorSourceMatches(&device,&source,t));
    source.Format.Graphics.PrimSurfSize.cx++;
    source.Format.Graphics.VisibleRegionSize.cy--; CHECK(!ConnectorSourceMatches(&device,&source,t));
    source.Format.Graphics.VisibleRegionSize.cy++;
    source.Format.Graphics.Stride++; CHECK(!ConnectorSourceMatches(&device,&source,t));
    source.Format.Graphics.Stride--;
    source.Format.Graphics.PixelFormat = 0; CHECK(!ConnectorSourceMatches(&device,&source,t));
    source.Format.Graphics.PixelFormat = formats[0];
    CHECK(ConnectorFindTiming(&device,&snapshot,&source,&signal) == t);
    ConnectorSignal(&snapshot.Sink.Targets[1],&bad);
    CHECK(ConnectorFindTiming(&device,&snapshot,&source,&bad) == &snapshot.Sink.Targets[1]);
    snapshot.Sink.Kind = BC250_SINK_NONE; CHECK(!ConnectorFindTiming(&device,&snapshot,NULL,NULL));
    snapshot.Sink.Kind = 1; snapshot.Sink.TargetCount = BC250_EDID_MAX_MODES+1;
    CHECK(!ConnectorFindTiming(&device,&snapshot,NULL,NULL));
    snapshot.Sink.TargetCount = 0; CHECK(!ConnectorFindTiming(&device,&snapshot,NULL,NULL));
    path.VidPnTargetId = BC250_CHILD_UID;
    path.ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
    path.ContentTransformation.Scaling = D3DKMDT_VPPS_IDENTITY;
    CHECK(ConnectorPathValid(&path));
    path.VidPnTargetId++; CHECK(!ConnectorPathValid(&path)); path.VidPnTargetId--;
    path.ContentTransformation.Rotation = 2; CHECK(!ConnectorPathValid(&path));
    path.ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
    path.ContentTransformation.Scaling = 2; CHECK(!ConnectorPathValid(&path));
    path.ContentTransformation.Scaling = D3DKMDT_VPPS_UNPINNED; CHECK(ConnectorPathValid(&path));
    path.VisibleFromActiveBROffset.cx = 1; CHECK(!ConnectorPathValid(&path));
    printf("connector modes: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
