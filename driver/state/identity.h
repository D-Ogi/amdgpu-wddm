#pragma once
// SHA-256 of the uppercase ASCII device instance ID, encoded as UTF-16LE without
// its NUL. PCI IDs are ASCII; reject other text instead of using locale casing.
// The digest binds a snapshot to a device; it does not authorize the snapshot.
unsigned int AmdgpuStateDeviceIdentity(const unsigned short *id,unsigned int characters,unsigned char hash[32]);
