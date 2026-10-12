#ifndef BC250_LINK_REGS_H
#define BC250_LINK_REGS_H
#include "bc250_atom_types.h"
/* Board-specific PHY-A/PLL0 policy for serialized ATOM roots 12, 76 and 4.
 * Combines the normal stream policy with finite cold-link register additions.
 * This is address admission, not authorization of arbitrary register values.
 * The caller must enforce known firmware, owner, root and execution budgets.
 */
int Bc250LinkRegisterAllowed(uint32_t dword, int write);
int Bc250LinkRegisterWriteNeedsBefore(uint32_t dword);
int Bc250LinkRegisterWriteValueAllowed(uint32_t dword, uint32_t before, uint32_t value);
/* Effective is valid only on success; refusal initializes it to zero and must
 * never reach the hardware write. Normalization reasons are a bit mask. */
enum BC250_LINK_WRITE_REASON {
    BC250_LINK_WRITE_UNCHANGED = 0,
    BC250_LINK_WRITE_COARSE_CLEAR = 1,
    BC250_LINK_WRITE_READONLY_STATUS = 2,
    BC250_LINK_WRITE_REFUSED_UNKNOWN = 4,
    BC250_LINK_WRITE_REFUSED_CONTROL = 8,
    BC250_LINK_WRITE_REFUSED_ADDRESS = 16,
    BC250_LINK_WRITE_INVALID_OUTPUT = 32
};
int Bc250LinkRegisterPrepareWrite(uint32_t dword, uint32_t before,
    uint32_t requested, uint32_t* effective, uint32_t* reason);
#endif
