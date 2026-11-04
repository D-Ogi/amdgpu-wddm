/* Positive control (the mutant) of tools/quality/kmd_ism.py (test_kmd_ism.py): the link of clean_state.c with
 * its phase spread over loose fields. Each pattern below is one candidate kind, and the test expects exactly
 * these keys:
 *   cluster _DEVICE.Active _DEVICE.Training   two flags that three transitions write together and one test reads
 *   cluster _DEVICE.Active _DEVICE.Faulted    the fault flag: one transition writes it with the two, one test
 *   cluster _DEVICE.Faulted _DEVICE.Training  reads it with the two (a third part of the same hidden phase)
 *   dead _DEVICE.LastError                   written, read nowhere
 *   sticky _DEVICE.Faulted                    set by a failure, cleared only by LinkStart
 *   int-state _DEVICE.Step                    an integer that holds the phase numbers 0, 1 and 2
 *   switch LinkName _LINK_KIND                no case for KindDvi
 * Never compiled into the driver. */
typedef unsigned char BOOLEAN;

typedef enum _LINK_KIND { KindDp, KindHdmi, KindDvi } LINK_KIND;

typedef struct _DEVICE {
    BOOLEAN Training;
    BOOLEAN Active;
    BOOLEAN Faulted;
    LINK_KIND Kind;
    unsigned long Step;
    unsigned long Errors;
    unsigned long LastError;
} DEVICE;

static DEVICE g_Device;

void LinkStart(DEVICE* d)
{
    d->Faulted = 0;
    d->Kind = KindDp;
}

static void LinkTrain(DEVICE* d)
{
    if (!d->Training && !d->Active) {
        d->Training = 1;
        d->Active = 0;
    }
}

static void LinkDone(DEVICE* d, int ok)
{
    if (!d->Training) return;
    d->Training = 0;
    d->Active = ok ? 1 : 0;
    if (!ok) {
        d->Errors++;
        d->LastError = 7;
        d->Faulted = 1;
    }
}

static void LinkDrop(DEVICE* d)
{
    d->Training = 0;
    d->Active = 0;
}

static void StepFirst(DEVICE* d) { d->Step = 0; }
static void StepSecond(DEVICE* d) { d->Step = 1; }
static void StepThird(DEVICE* d) { d->Step = 2; }

int LinkUsable(const DEVICE* d)
{
    return d->Active && !d->Training && !d->Faulted && d->Step == 2 && d->Errors < 3;
}

const char* LinkName(const DEVICE* d)
{
    switch (d->Kind) {
    case KindDp:
        return "DP";
    case KindHdmi:
        return "HDMI";
    default:
        return "?";
    }
}

int Drive(void)
{
    LinkStart(&g_Device);
    StepFirst(&g_Device);
    LinkTrain(&g_Device);
    StepSecond(&g_Device);
    LinkDone(&g_Device, 1);
    StepThird(&g_Device);
    LinkDrop(&g_Device);
    return LinkUsable(&g_Device) + (LinkName(&g_Device)[0] == 'D');
}
