// Narrow, typed ATOM display commands for a serialized DCN201 DIG0/PHY-A owner.
#pragma once
#include "bc250_atom.h"

enum bc250_link_protocol { BC250_LINK_DP=0, BC250_LINK_DVI=2, BC250_LINK_HDMI=3 };
enum bc250_link_tx_action { BC250_LINK_TX_OFF, BC250_LINK_TX_ON, BC250_LINK_TX_DRIVE };
typedef struct BC250_LINK_ATOM {
    const BC250_ATOM_IMAGE* Image;
    BC250_ATOM_PLAN* Plan;
    void* Workspace;
    size_t WorkspaceSize;
    BC250_ATOM_CALLBACKS Io;
    BC250_ATOM_RESULT Last;
    unsigned Ready,Commands;
    /* Internal execution scope, cleared before returning to the caller. */
    unsigned ClockScope;
} BC250_LINK_ATOM;

// Checks all three table versions and call trees, with no register callbacks.
BC250_ATOM_STATUS Bc250LinkAtomValidate(BC250_LINK_ATOM* Link);
BC250_ATOM_STATUS Bc250LinkAtomReferenceClock(const BC250_ATOM_IMAGE* Image,unsigned* CrystalKhz);
BC250_ATOM_STATUS Bc250LinkAtomClock(BC250_LINK_ATOM* Link,unsigned Protocol,unsigned PixelKHz);
BC250_ATOM_STATUS Bc250LinkAtomTransmitter(BC250_LINK_ATOM* Link,enum bc250_link_tx_action Action,
    unsigned Protocol,unsigned SymbolKHz,unsigned Lanes,unsigned Swing,unsigned PreEmphasis);
BC250_ATOM_STATUS Bc250LinkAtomStream(BC250_LINK_ATOM* Link,unsigned Protocol,
    unsigned PixelKHz,unsigned Lanes,unsigned Rate);

/* Kernel transports retain owner/MMIO checks and OR this narrow scope with the
 * generic register policy. It is not a replacement for PSP lifetime exclusion. */
int Bc250LinkAtomScopedRegister(const BC250_LINK_ATOM* Link,uint32_t Dword,int Write);
