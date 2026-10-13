// The VidPN manager boundary: sets, pin state, borrowed objects and injected
// transport failures. No predicate or mode enumeration is reimplemented here.
typedef unsigned long D3DDDI_VIDEO_PRESENT_SOURCE_ID;
typedef unsigned long D3DDDI_VIDEO_PRESENT_TARGET_ID;
typedef void* D3DKMDT_HVIDPN;
typedef void* D3DKMDT_HVIDPNTOPOLOGY;
typedef void* D3DKMDT_HVIDPNSOURCEMODESET;
typedef void* D3DKMDT_HVIDPNTARGETMODESET;
enum {D3DKMDT_CB_SCRGB=2,D3DKMDT_PVAM_DIRECT=1,D3DKMDT_MP_PREFERRED=1,D3DKMDT_BITS_PER_COMPONENT_08=1};
enum {D3DKMDT_EPT_UNINITIALIZED=0,D3DKMDT_EPT_VIDPNSOURCE=1,D3DKMDT_EPT_VIDPNTARGET=2,
    D3DKMDT_EPT_SCALING=3,D3DKMDT_EPT_ROTATION=4,D3DKMDT_EPT_NOPIVOT=5};
#define STATUS_GRAPHICS_MODE_NOT_PINNED 1L
#define STATUS_GRAPHICS_DATASET_IS_EMPTY 2L
#define STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET 3L
#define STATUS_GRAPHICS_INVALID_PIXELFORMAT (-10L)
#define STATUS_GRAPHICS_INVALID_STRIDE (-11L)
#define D3DDDIFMT_A8R8G8B8 21
typedef struct {D3DKMDT_VIDEO_SIGNAL_INFO VideoSignalInfo;
    struct {unsigned Value,Preference,Rgb;} WireFormatAndPreference;} D3DKMDT_VIDPN_TARGET_MODE;
typedef struct {D3DKMDT_HVIDPN hDesiredVidPn;BOOLEAN IsVidPnSupported;} DXGKARG_ISSUPPORTEDVIDPN;
typedef struct {D3DKMDT_HVIDPN hConstrainingVidPn;unsigned EnumPivotType;
    struct {ULONG VidPnSourceId,VidPnTargetId;} EnumPivot;} DXGKARG_ENUMVIDPNCOFUNCMODALITY;
typedef struct {
    NTSTATUS (*pfnCreateNewModeInfo)(void*,D3DKMDT_VIDPN_SOURCE_MODE**);
    NTSTATUS (*pfnAddMode)(void*,D3DKMDT_VIDPN_SOURCE_MODE*);
    NTSTATUS (*pfnReleaseModeInfo)(void*,const D3DKMDT_VIDPN_SOURCE_MODE*);
    NTSTATUS (*pfnAcquirePinnedModeInfo)(void*,const D3DKMDT_VIDPN_SOURCE_MODE**);
} DXGK_VIDPNSOURCEMODESET_INTERFACE;
typedef struct {
    NTSTATUS (*pfnCreateNewModeInfo)(void*,D3DKMDT_VIDPN_TARGET_MODE**);
    NTSTATUS (*pfnAddMode)(void*,D3DKMDT_VIDPN_TARGET_MODE*);
    NTSTATUS (*pfnReleaseModeInfo)(void*,const D3DKMDT_VIDPN_TARGET_MODE*);
    NTSTATUS (*pfnAcquirePinnedModeInfo)(void*,const D3DKMDT_VIDPN_TARGET_MODE**);
} DXGK_VIDPNTARGETMODESET_INTERFACE;
typedef struct {
    NTSTATUS (*pfnGetNumPaths)(void*,SIZE_T*);
    NTSTATUS (*pfnAcquireFirstPathInfo)(void*,const D3DKMDT_VIDPN_PRESENT_PATH**);
    NTSTATUS (*pfnReleasePathInfo)(void*,const D3DKMDT_VIDPN_PRESENT_PATH*);
    NTSTATUS (*pfnAcquireNextPathInfo)(void*,const D3DKMDT_VIDPN_PRESENT_PATH*,const D3DKMDT_VIDPN_PRESENT_PATH**);
    NTSTATUS (*pfnUpdatePathSupportInfo)(void*,const D3DKMDT_VIDPN_PRESENT_PATH*);
} DXGK_VIDPNTOPOLOGY_INTERFACE;
typedef struct TEST_VIDPN_INTERFACE {
    NTSTATUS (*pfnGetTopology)(void*,void**,const DXGK_VIDPNTOPOLOGY_INTERFACE**);
    NTSTATUS (*pfnCreateNewSourceModeSet)(void*,ULONG,void**,const DXGK_VIDPNSOURCEMODESET_INTERFACE**);
    NTSTATUS (*pfnAssignSourceModeSet)(void*,ULONG,void*);
    NTSTATUS (*pfnReleaseSourceModeSet)(void*,void*);
    NTSTATUS (*pfnCreateNewTargetModeSet)(void*,ULONG,void**,const DXGK_VIDPNTARGETMODESET_INTERFACE**);
    NTSTATUS (*pfnAssignTargetModeSet)(void*,ULONG,void*);
    NTSTATUS (*pfnReleaseTargetModeSet)(void*,void*);
    NTSTATUS (*pfnAcquireSourceModeSet)(void*,ULONG,void**,const DXGK_VIDPNSOURCEMODESET_INTERFACE**);
    NTSTATUS (*pfnAcquireTargetModeSet)(void*,ULONG,void**,const DXGK_VIDPNTARGETMODESET_INTERFACE**);
} DXGK_VIDPN_INTERFACE;
typedef struct {void* hFunctionalVidPn;ULONG AffectedVidPnSourceId;
    struct {unsigned PathPowerTransition,PathPoweredOff;} Flags;} DXGKARG_COMMITVIDPN;
#define DXGK_VIDPN_INTERFACE_VERSION_V1 1
#define StageFirstCommitVidPn 1
static D3DKMDT_VIDPN_SOURCE_MODE commitSource,offeredSources[BC250_SOURCE_MODE_MAX];
static D3DKMDT_VIDPN_PRESENT_PATH commitPath,borrowedPath;
static D3DKMDT_VIDEO_SIGNAL_INFO commitTarget;
static D3DKMDT_VIDPN_TARGET_MODE offeredTarget,pinnedTarget;
static SIZE_T commitPaths=1;
static BOOLEAN sourceIsPinned=TRUE,targetIsPinned=TRUE;
static unsigned sourceCount,targetCount,sourceAssigned,targetAssigned,updateCalls;
static unsigned pathRefs,sourceRefs,targetRefs,sourceInfoRefs,targetInfoRefs;
static unsigned failBoundary;
static NTSTATUS CommitGetNumPaths(void* h,SIZE_T* count){(void)h;*count=commitPaths;return 0;}
static NTSTATUS CommitGetPath(void* h,const D3DKMDT_VIDPN_PRESENT_PATH** p)
{(void)h;if(!commitPaths){*p=NULL;return STATUS_GRAPHICS_DATASET_IS_EMPTY;}
    borrowedPath=commitPath;*p=&borrowedPath;pathRefs++;return 0;}
static NTSTATUS CommitReleasePath(void* h,const D3DKMDT_VIDPN_PRESENT_PATH* p)
{(void)h;(void)p;pathRefs--;return 0;}
static NTSTATUS CommitNextPath(void* h,const D3DKMDT_VIDPN_PRESENT_PATH* p,const D3DKMDT_VIDPN_PRESENT_PATH** next)
{(void)h;(void)p;*next=NULL;return STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET;}
static NTSTATUS CommitUpdatePath(void* h,const D3DKMDT_VIDPN_PRESENT_PATH* p)
{(void)h;if(failBoundary==5)return STATUS_ACCESS_DENIED;commitPath=*p;updateCalls++;return 0;}
static const DXGK_VIDPNTOPOLOGY_INTERFACE commitTopology={CommitGetNumPaths,CommitGetPath,CommitReleasePath,
    CommitNextPath,CommitUpdatePath};
static NTSTATUS CommitGetTopology(void* h,void** topology,const DXGK_VIDPNTOPOLOGY_INTERFACE** iface)
{*topology=h;*iface=&commitTopology;return 0;}
static NTSTATUS CreateSourceInfo(void* h,D3DKMDT_VIDPN_SOURCE_MODE** mode)
{(void)h;if(sourceCount>=BC250_SOURCE_MODE_MAX)abort();*mode=&offeredSources[sourceCount];sourceInfoRefs++;return 0;}
static NTSTATUS AddSourceInfo(void* h,D3DKMDT_VIDPN_SOURCE_MODE* mode)
{(void)h;(void)mode;if(failBoundary==3)return STATUS_ACCESS_DENIED;sourceCount++;sourceInfoRefs--;return 0;}
static NTSTATUS ReleaseSourceInfo(void* h,const D3DKMDT_VIDPN_SOURCE_MODE* mode)
{(void)h;(void)mode;sourceInfoRefs--;return 0;}
static NTSTATUS PinnedSourceInfo(void* h,const D3DKMDT_VIDPN_SOURCE_MODE** mode)
{(void)h;*mode=sourceIsPinned?&commitSource:NULL;if(*mode)sourceInfoRefs++;return sourceIsPinned?0:STATUS_GRAPHICS_MODE_NOT_PINNED;}
static const DXGK_VIDPNSOURCEMODESET_INTERFACE sourceInterface={CreateSourceInfo,AddSourceInfo,ReleaseSourceInfo,PinnedSourceInfo};
static NTSTATUS AcquireSource(void* h,ULONG id,void** set,const DXGK_VIDPNSOURCEMODESET_INTERFACE** iface)
{(void)h;if(id!=0 || failBoundary==1)return STATUS_ACCESS_DENIED;*set=&sourceCount;*iface=&sourceInterface;sourceRefs++;return 0;}
static NTSTATUS CreateSource(void* h,ULONG id,void** set,const DXGK_VIDPNSOURCEMODESET_INTERFACE** iface)
{sourceCount=0;memset(offeredSources,0,sizeof(offeredSources));return AcquireSource(h,id,set,iface);}
static NTSTATUS AssignSource(void* h,ULONG id,void* set)
{(void)h;(void)id;(void)set;sourceRefs--;sourceAssigned++;return 0;}
static NTSTATUS ReleaseSource(void* h,void* set){(void)h;(void)set;sourceRefs--;return 0;}
static NTSTATUS CreateTargetInfo(void* h,D3DKMDT_VIDPN_TARGET_MODE** mode)
{(void)h;*mode=&offeredTarget;targetInfoRefs++;return 0;}
static NTSTATUS AddTargetInfo(void* h,D3DKMDT_VIDPN_TARGET_MODE* mode)
{(void)h;(void)mode;if(failBoundary==4)return STATUS_ACCESS_DENIED;targetCount++;targetInfoRefs--;return 0;}
static NTSTATUS ReleaseTargetInfo(void* h,const D3DKMDT_VIDPN_TARGET_MODE* mode)
{(void)h;(void)mode;targetInfoRefs--;return 0;}
static NTSTATUS PinnedTargetInfo(void* h,const D3DKMDT_VIDPN_TARGET_MODE** mode)
{(void)h;pinnedTarget.VideoSignalInfo=commitTarget;*mode=targetIsPinned?&pinnedTarget:NULL;
    if(*mode)targetInfoRefs++;return targetIsPinned?0:STATUS_GRAPHICS_MODE_NOT_PINNED;}
static const DXGK_VIDPNTARGETMODESET_INTERFACE targetInterface={CreateTargetInfo,AddTargetInfo,ReleaseTargetInfo,PinnedTargetInfo};
static NTSTATUS AcquireTarget(void* h,ULONG id,void** set,const DXGK_VIDPNTARGETMODESET_INTERFACE** iface)
{(void)h;if(id!=BC250_CHILD_UID || failBoundary==2)return STATUS_ACCESS_DENIED;
    *set=&targetCount;*iface=&targetInterface;targetRefs++;return 0;}
static NTSTATUS CreateTarget(void* h,ULONG id,void** set,const DXGK_VIDPNTARGETMODESET_INTERFACE** iface)
{targetCount=0;memset(&offeredTarget,0,sizeof(offeredTarget));return AcquireTarget(h,id,set,iface);}
static NTSTATUS AssignTarget(void* h,ULONG id,void* set)
{(void)h;(void)id;(void)set;targetRefs--;targetAssigned++;return 0;}
static NTSTATUS ReleaseTarget(void* h,void* set){(void)h;(void)set;targetRefs--;return 0;}
static const DXGK_VIDPN_INTERFACE commitInterface={CommitGetTopology,CreateSource,AssignSource,ReleaseSource,
    CreateTarget,AssignTarget,ReleaseTarget,AcquireSource,AcquireTarget};
static NTSTATUS CommitQueryInterface(void* h,unsigned version,const DXGK_VIDPN_INTERFACE** iface)
{(void)h;(void)version;*iface=&commitInterface;return 0;}
static void DisplayRetainCommitPower(BC250_DEVICE* d,const DXGKARG_COMMITVIDPN* args){(void)d;(void)args;}
static void GuardStage(unsigned stage){(void)stage;}
static NTSTATUS ConnectorVidPn(BC250_DEVICE* d,void* h,const void* e,BOOLEAN commit,BOOLEAN* supported)
{(void)d;(void)h;(void)e;(void)commit;(void)supported;UnexpectedConnector();return STATUS_NOT_SUPPORTED;}
static ULONG DcnPrimaryPitch(ULONG width){return (width*4+255)&~255ul;}
