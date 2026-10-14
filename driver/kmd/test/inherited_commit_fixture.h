// The real ModesetCommit body runs below. A scaler transaction here is an
// observed failure, not an emulation of successful hardware programming.
typedef long LONG;
#define STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE (-8L)
#define STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED (-9L)
static unsigned commitTransactions,commitPrograms;
static ULONG DisplaySourceWidth(const BC250_DEVICE* d){return d->Modeset.SourceWidth?(ULONG)d->Modeset.SourceWidth:d->Post.Width;}
static ULONG DisplaySourceHeight(const BC250_DEVICE* d){return d->Modeset.SourceHeight?(ULONG)d->Modeset.SourceHeight:d->Post.Height;}
static void ExAcquireFastMutex(int* lock){++*lock;}
static void ExReleaseFastMutex(int* lock){--*lock;}
static LONG InterlockedExchange(LONG* value,LONG next){LONG old=*value;*value=next;return old;}
static LONG InterlockedIncrement(volatile LONG* value){return ++*value;}
static BOOLEAN WddmPrimaryExclusiveBegin(BC250_DEVICE* d,ULONG kind,ULONG* gen)
{(void)d;(void)kind;*gen=0;commitTransactions++;return TRUE;}
static void WddmPrimaryExclusiveEnd(BC250_DEVICE* d,ULONG gen,BOOLEAN invalid)
{(void)d;(void)gen;(void)invalid;}
static long ProgramLocked(BC250_DEVICE* d,const BC250_SCALER_PLAN* p,BOOLEAN grow,BOOLEAN* invalid)
{(void)d;(void)p;(void)grow;*invalid=FALSE;commitPrograms++;return STATUS_NOT_SUPPORTED;}
