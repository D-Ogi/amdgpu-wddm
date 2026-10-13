// VidPN manager boundary for the unchanged CommitVidPnCore body. The pinned
// Windows objects are inputs, while all source/target validation remains real.
typedef void* D3DKMDT_HVIDPN;
typedef void* D3DKMDT_HVIDPNTOPOLOGY;
typedef struct {
    NTSTATUS (*pfnGetNumPaths)(D3DKMDT_HVIDPNTOPOLOGY,SIZE_T*);
    NTSTATUS (*pfnAcquireFirstPathInfo)(D3DKMDT_HVIDPNTOPOLOGY,const D3DKMDT_VIDPN_PRESENT_PATH**);
    NTSTATUS (*pfnReleasePathInfo)(D3DKMDT_HVIDPNTOPOLOGY,const D3DKMDT_VIDPN_PRESENT_PATH*);
} DXGK_VIDPNTOPOLOGY_INTERFACE;
typedef struct TEST_VIDPN_INTERFACE {
    NTSTATUS (*pfnGetTopology)(D3DKMDT_HVIDPN,D3DKMDT_HVIDPNTOPOLOGY*,const DXGK_VIDPNTOPOLOGY_INTERFACE**);
} DXGK_VIDPN_INTERFACE;
typedef struct { D3DKMDT_HVIDPN hFunctionalVidPn;ULONG AffectedVidPnSourceId;
    struct {unsigned PathPowerTransition,PathPoweredOff;} Flags;} DXGKARG_COMMITVIDPN;
#define DXGK_VIDPN_INTERFACE_VERSION_V1 1
#define StageFirstCommitVidPn 1
static D3DKMDT_VIDPN_SOURCE_MODE commitSource;
static D3DKMDT_VIDPN_PRESENT_PATH commitPath;
static D3DKMDT_VIDEO_SIGNAL_INFO commitTarget;
static SIZE_T commitPaths=1;
static NTSTATUS CommitGetNumPaths(D3DKMDT_HVIDPNTOPOLOGY h,SIZE_T* count)
{(void)h;*count=commitPaths;return 0;}
static NTSTATUS CommitGetPath(D3DKMDT_HVIDPNTOPOLOGY h,const D3DKMDT_VIDPN_PRESENT_PATH** p)
{(void)h;*p=&commitPath;return 0;}
static NTSTATUS CommitReleasePath(D3DKMDT_HVIDPNTOPOLOGY h,const D3DKMDT_VIDPN_PRESENT_PATH* p)
{(void)h;(void)p;return 0;}
static const DXGK_VIDPNTOPOLOGY_INTERFACE commitTopology={CommitGetNumPaths,CommitGetPath,CommitReleasePath};
static NTSTATUS CommitGetTopology(D3DKMDT_HVIDPN h,D3DKMDT_HVIDPNTOPOLOGY* topology,
    const DXGK_VIDPNTOPOLOGY_INTERFACE** iface)
{*topology=h;*iface=&commitTopology;return 0;}
static const DXGK_VIDPN_INTERFACE commitInterface={CommitGetTopology};
static NTSTATUS CommitQueryInterface(D3DKMDT_HVIDPN h,unsigned version,const DXGK_VIDPN_INTERFACE** iface)
{(void)h;(void)version;*iface=&commitInterface;return 0;}
static NTSTATUS SourceModeIsPinned(const DXGK_VIDPN_INTERFACE* iface,D3DKMDT_HVIDPN h,ULONG id,
    BOOLEAN* pinned,D3DKMDT_VIDPN_SOURCE_MODE* mode)
{(void)iface;(void)h;if(id!=0)return STATUS_INVALID_PARAMETER;*pinned=TRUE;*mode=commitSource;return 0;}
static NTSTATUS TargetModeIsPinned(const DXGK_VIDPN_INTERFACE* iface,D3DKMDT_HVIDPN h,ULONG id,
    BOOLEAN* pinned,D3DKMDT_VIDEO_SIGNAL_INFO* signal)
{(void)iface;(void)h;if(id!=BC250_CHILD_UID)return STATUS_INVALID_PARAMETER;*pinned=TRUE;*signal=commitTarget;return 0;}
static void DisplayRetainCommitPower(BC250_DEVICE* d,const DXGKARG_COMMITVIDPN* args){(void)d;(void)args;}
static void GuardStage(unsigned stage){(void)stage;}
static NTSTATUS ConnectorVidPn(BC250_DEVICE* d,D3DKMDT_HVIDPN h,const void* e,BOOLEAN commit,BOOLEAN* supported)
{(void)d;(void)h;(void)e;(void)commit;(void)supported;UnexpectedConnector();return STATUS_NOT_SUPPORTED;}
