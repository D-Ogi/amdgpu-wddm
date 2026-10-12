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
