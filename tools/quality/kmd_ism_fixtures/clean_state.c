/* Negative control of tools/quality/kmd_ism.py (test_kmd_ism.py): one explicit state machine. The link has
 * one enum field that names every phase, every transition writes it, the switch handles every value, and the
 * error counter is read. The detector must report nothing here. loose_flags.c is the same link written with
 * loose flags, and there the detector must report each kind of candidate. Never compiled into the driver. */
typedef unsigned char BOOLEAN;

typedef enum _LINK_PHASE { LinkIdle, LinkTraining, LinkActive, LinkFailed } LINK_PHASE;

typedef struct _DEVICE {
    LINK_PHASE Phase;
    unsigned long Errors;
} DEVICE;

static DEVICE g_Device;

static void LinkTrain(DEVICE* d)
{
    if (d->Phase == LinkIdle) d->Phase = LinkTraining;
}

static void LinkDone(DEVICE* d, int ok)
{
    if (d->Phase != LinkTraining) return;
    if (ok) {
        d->Phase = LinkActive;
    } else {
        d->Phase = LinkFailed;
        d->Errors++;
    }
}

static void LinkDrop(DEVICE* d)
{
    d->Phase = LinkIdle;
}

int LinkUsable(const DEVICE* d)
{
    switch (d->Phase) {
    case LinkIdle:
    case LinkTraining:
    case LinkFailed:
        return 0;
    case LinkActive:
        return 1;
    }
    return 0;
}

unsigned long LinkErrors(const DEVICE* d)
{
    return d->Errors;
}

int Drive(void)
{
    LinkTrain(&g_Device);
    LinkDone(&g_Device, 1);
    LinkDrop(&g_Device);
    return LinkUsable(&g_Device) + (int)LinkErrors(&g_Device);
}
