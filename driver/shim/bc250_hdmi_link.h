/* Same-timing DVI -> HDMI conversion on an already powered DCN201 DIG0 link.
 * No PHY/PLL writes, no timing writes, no hot-plug. The caller owns PnP lifetime
 * and excludes flips. Failure requires Restore, even if the final write failed. */
#ifndef BC250_HDMI_LINK_H
#define BC250_HDMI_LINK_H
#include "bc250_atom.h"

#define BC250_HDMI_LINK_REGS 48u
struct BC250_HDMI_PREFLIGHT;
typedef struct BC250_HDMI_RESTORE_DETAIL {
    uint32_t Valid, Operation, Register, Wanted, Readback, Mask, ReadbackValid;
} BC250_HDMI_RESTORE_DETAIL;
typedef struct BC250_HDMI_LINK {
    BC250_ATOM_CALLBACKS Io;
    uint32_t Offset[BC250_HDMI_LINK_REGS], Before[BC250_HDMI_LINK_REGS];
    unsigned Count, Dirty, Restoring, PacketDirty, Active, PacketAdmitted;
    uint32_t FailureRegister, RestoreRegister;
    uint32_t FailureOperation, FailureWanted, FailureReadback, FailureMask;
    int Error;
    BC250_ATOM_RESULT Atom;
    /* Private to the shadow evaluator. Live Start always resets this pointer. */
    struct BC250_HDMI_PREFLIGHT* Shadow;
    /* First restore failure, retained across retries separately from Start.
     * Readback is meaningful only when ReadbackValid is nonzero. */
    BC250_HDMI_RESTORE_DETAIL RestoreFailure;
    /* Runtime stream failure, retained across Restore. Operations 1..6 match
     * the register failures. Operation 7 is invalid arguments (no readback);
     * operation 8 reports software Dirty, with Register=0. */
    BC250_HDMI_RESTORE_DETAIL StreamFailure;
    /* 1 admission, 2 route, 3 mute, 4 attributes, 5 RAM power, 6 AVI, 7 unmute. */
    uint32_t StreamStage;
} BC250_HDMI_LINK;

/* Direct callbacks use DWORD offsets, like ATOM. Allowed is independently
 * enforced here and again by the hardware wrapper. No parameter selects a register. */
int Bc250HdmiLinkAllowed(uint32_t dword, int write);
/* Exhaustive generated policy; unknown fields are not evidence of writability. */
typedef struct BC250_HDMI_FIELD_POLICY {
    uint32_t Register, Configuration, Command, Status, Unknown, PacketData;
} BC250_HDMI_FIELD_POLICY;
const BC250_HDMI_FIELD_POLICY *Bc250HdmiLinkPolicy(uint32_t dword);
unsigned Bc250HdmiLinkPolicyCount(void);
const BC250_HDMI_FIELD_POLICY *Bc250HdmiLinkPolicyAt(unsigned index);

#define BC250_HDMI_SNAPSHOT_REGS 32u
#define BC250_HDMI_PREFLIGHT_REFUSALS 64u
typedef struct BC250_HDMI_SNAPSHOT_ENTRY {
    uint32_t Register, Value;
    int Status; /* zero only after a successful read; unreadable is never zero data */
} BC250_HDMI_SNAPSHOT_ENTRY;
typedef struct BC250_HDMI_SNAPSHOT {
    unsigned Count, Complete;
    BC250_HDMI_SNAPSHOT_ENTRY Entries[BC250_HDMI_SNAPSHOT_REGS];
} BC250_HDMI_SNAPSHOT;
typedef struct BC250_HDMI_REFUSAL {
    uint32_t Register, Operation, Wanted, Before, Mask;
} BC250_HDMI_REFUSAL;
typedef struct BC250_HDMI_PREFLIGHT {
    unsigned Complete, Fatal, Count, Events, Overflow;
    unsigned Reads, Writes, DelayUs;
    uint64_t ElapsedUs;
    BC250_HDMI_REFUSAL Refusals[BC250_HDMI_PREFLIGHT_REFUSALS];
    BC250_HDMI_REFUSAL Last;
    BC250_ATOM_RESULT Atom;
} BC250_HDMI_PREFLIGHT;
/* Captures configuration/control registers and read-only entry prerequisites,
 * including each read failure. Indexed packet data is not configuration and
 * has no immediate-readback guarantee in the imported AMD sequence.
 * No write or delay callback is invoked. Caller excludes concurrent owners. */
int Bc250HdmiLinkSnapshot(const BC250_ATOM_CALLBACKS* io,BC250_HDMI_SNAPSHOT* snapshot);
size_t Bc250HdmiLinkShadowSize(void);
/* Runs the same Start sequence against a private sampled register model.
 * No live callbacks are accepted. Policy refusals are collected while following
 * the intended writes; that counterfactual path is diagnostic, not admission.
 * A zero return requires a complete path and no refusals, overflow or fatal
 * error. It does not prove hardware command completion or replace live checks.
 * All workspace and result storage belongs to the caller, normally nonpaged pool. */
int Bc250HdmiLinkPreflight(const BC250_HDMI_SNAPSHOT* snapshot,
    const BC250_ATOM_IMAGE* image,BC250_ATOM_PLAN* plan,void* atomWorkspace,size_t atomBytes,
    void* shadowWorkspace,size_t shadowBytes,uint32_t pixelKHz,unsigned width,unsigned height,
    unsigned htotal,unsigned vtotal,BC250_HDMI_PREFLIGHT* result);
int Bc250HdmiLinkStart(BC250_HDMI_LINK *link, const BC250_ATOM_CALLBACKS *io,
    const BC250_ATOM_IMAGE *image, BC250_ATOM_PLAN *plan, void *workspace,
    size_t workspaceSize, uint32_t pixelKHz, unsigned width, unsigned height,
    unsigned htotal, unsigned vtotal);
int Bc250HdmiLinkRestore(BC250_HDMI_LINK *link);
/* Runtime cold modeset: outer owner selects DIG0 HDMI/FE0 with peer disabled.
 * TX may still be disabled; this programs RGB8 attributes/AVI and leaves AVMUTE.
 * Active packet 0 can be retired only with TX and OTG stopped. Its stopped
 * controls form the restore baseline; its former RAM is not sampled or replayed.
 * All other packet slots remain untouched. Only controls are restored on exit;
 * a previous HDMI mode must rebuild its AVI from that mode's known timing.
 * Packet data write completion needs hardware validation, not an MMIO echo.
 * No ATOM, PHY, PLL or timing operation is performed by this entry point. */
int Bc250HdmiStreamProgram(BC250_HDMI_LINK *link,const BC250_ATOM_CALLBACKS *io,
    uint32_t pixelKHz,unsigned width,unsigned height,unsigned htotal,unsigned vtotal);
/* Unmute only after outer-owner TX enable and plane unblank; hardware TX enable
 * and symclk are checked here. Mute remains valid before/after TX enable. */
int Bc250HdmiStreamMute(BC250_HDMI_LINK *link,int muted);
/* Pure packer, used by the stream and by the known-vector host gate. */
void Bc250HdmiLinkAvi(unsigned width, unsigned height, unsigned htotal,
    unsigned vtotal, uint8_t header[4], uint8_t payload[32]);
#endif
