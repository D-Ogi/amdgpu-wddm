#include <ntddk.h>
#include "adapter_owner.h"
static void* volatile AdapterOwner;
static unsigned char AdapterPoison;
int AdapterOwnerClaim(void* identity)
{
    return identity && identity != &AdapterPoison &&
        InterlockedCompareExchangePointer(&AdapterOwner, identity, NULL) == NULL;
}
int AdapterOwnerIs(void* identity)
{
    return identity && identity != &AdapterPoison &&
        InterlockedCompareExchangePointer(&AdapterOwner, NULL, NULL) == identity;
}
void AdapterOwnerRelease(void* identity, int uncertain)
{
    if (identity && identity != &AdapterPoison)
        (void)InterlockedCompareExchangePointer(&AdapterOwner,
            uncertain ? &AdapterPoison : NULL, identity);
}
