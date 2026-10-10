#pragma once
/* One PDO owns this driver image from AddDevice through completed RemoveDevice.
   Stop/start and retained power keep ownership. An uncertain stop permanently
   poisons admission until image unload; no freed device pointer remains stored. */
int AdapterOwnerClaim(void* identity);
int AdapterOwnerIs(void* identity);
void AdapterOwnerRelease(void* identity, int uncertain);
