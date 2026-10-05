from pathlib import Path
r=Path('P:/bc-250/bc250-win/driver/kmd/test')
p=r/'generate_display_visibility_test.py';s=p.read_text().replace("('wddm.c',['void WddmSourceVisibility('])", "('wddm.c',['static void WddmVSyncArm(', 'void WddmSourceVisibility('])");p.write_text(s)
p=r/'display_visibility_test.c';s=p.read_text();s=s.replace('typedef void* HANDLE;', '''typedef void* HANDLE;
typedef int KIRQL;
typedef int64_t LONGLONG;
typedef struct {LONGLONG QuadPart;} LARGE_INTEGER;
typedef struct {int Lock,Stopping,VSyncArmed,VSyncTimer,VSyncDpc;long VSyncTicks;} BC250_WDDM;''');s=s.replace('    void *Mmio,*Framebuffer;size_t FramebufferLength;', '    BC250_WDDM* Wddm;\n    void *Mmio,*Framebuffer;size_t FramebufferLength;');s=s.replace('#define TRUE 1', '#define BC250_WDDM_VSYNC_MS 16\n#define GuardLog(...) ((void)0)\n#define TRUE 1')
s=s.replace('static void WddmVSyncArm(BC250_DEVICE*d,BOOLEAN on){CHECK(on);model.arms++;CHECK(DcnVsyncEnable(d,on)==STATUS_SUCCESS);}', '''static void KeAcquireSpinLock(int*l,KIRQL*i){(void)l;CHECK(!model.irq);*i=0;model.arms++;}
static void KeReleaseSpinLock(int*l,KIRQL i){(void)l;(void)i;CHECK(!model.irq);}
static void KeSetTimerEx(int*t,LARGE_INTEGER due,unsigned period,int*d){(void)t;(void)due;(void)period;(void)d;}
static void KeCancelTimer(int*t){(void)t;}''')
s=s.replace('    BC250_DEVICE d={0};DXGKARG_SETVIDPNSOURCEVISIBILITY visible={0};','    BC250_DEVICE d={0};BC250_WDDM w={0};DXGKARG_SETVIDPNSOURCEVISIBILITY visible={0};');s=s.replace('    d.Mmio=&model;d.Framebuffer=fb;', '    d.Wddm=&w;d.Mmio=&model;d.Framebuffer=fb;');p.write_text(s)
