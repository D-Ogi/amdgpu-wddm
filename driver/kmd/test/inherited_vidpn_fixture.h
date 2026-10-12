// Only fields the extracted DDI predicate reads. Production is also compiled
// against the SDK/WDK declarations; these stubs do not model callback ABI.
#include "display_modes.h"
typedef unsigned D3DKMDT_VIDPN_PRESENT_PATH_SCALING;
enum {D3DKMDT_VPPS_UNINITIALIZED=0,D3DKMDT_VPPS_IDENTITY=1,D3DKMDT_VPPS_CENTERED=2,
      D3DKMDT_VPPS_STRETCHED=3,D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX=4,D3DKMDT_VPPS_CUSTOM=5,
      D3DKMDT_VPPS_UNPINNED=254,D3DKMDT_VPPS_NOTSPECIFIED=255};
enum {D3DKMDT_VPPR_UNINITIALIZED=0,D3DKMDT_VPPR_IDENTITY=1,D3DKMDT_VPPR_UNPINNED=254};
enum {D3DKMDT_RMT_GRAPHICS=1};
typedef struct {unsigned Type;struct {struct {REGION PrimSurfSize,VisibleRegionSize;ULONG Stride,PixelFormat;} Graphics;} Format;} D3DKMDT_VIDPN_SOURCE_MODE;
typedef struct {unsigned VidPnSourceId,VidPnTargetId;struct {unsigned Scaling,Rotation;} ContentTransformation;
    REGION VisibleFromActiveTLOffset,VisibleFromActiveBROffset;} D3DKMDT_VIDPN_PRESENT_PATH;
typedef struct {D3DKMDT_VIDPN_PRESENT_PATH VidPnPresentPathInfo;} DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH;
typedef struct {ULONG Generation;} BC250_CONNECTOR_SNAPSHOT;
static BC250_CONNECTOR_SNAPSHOT* ConnectorSnapshotAcquire(BC250_DEVICE* d)
{(void)d;UnexpectedConnector();return NULL;}
static void ConnectorSnapshotRelease(BC250_CONNECTOR_SNAPSHOT* s){(void)s;UnexpectedConnector();}
static BOOLEAN ConnectorPathValid(const D3DKMDT_VIDPN_PRESENT_PATH* p){(void)p;UnexpectedConnector();return FALSE;}
static const BC250_TARGET_TIMING* ConnectorFindTiming(const BC250_DEVICE* d,
    const BC250_CONNECTOR_SNAPSHOT* s,const D3DKMDT_VIDPN_SOURCE_MODE* m,const D3DKMDT_VIDEO_SIGNAL_INFO* v)
{(void)d;(void)s;(void)m;(void)v;UnexpectedConnector();return NULL;}
static BOOLEAN ConnectorGenerationCurrent(BC250_DEVICE* d,ULONG g){(void)d;(void)g;UnexpectedConnector();return FALSE;}
