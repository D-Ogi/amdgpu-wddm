// SPDX-License-Identifier: MIT
#pragma once
// Optional trailer following the unchanged v3 UMDRIVERPRIVATE caps blob.
// Query a zero-initialized extended buffer and require all header fields before
// using the LUID: older KMDs succeed but only write the legacy prefix.
#define BC250_ADAPTER_IDENTITY_OFFSET 1472u
#define BC250_ADAPTER_IDENTITY_MAGIC 0x49413242u /* B2AI */
#define BC250_ADAPTER_IDENTITY_VERSION 1u
#define BC250_ADAPTER_IDENTITY_BYTES 24u
#define BC250_ADAPTER_CAPS_BYTES (BC250_ADAPTER_IDENTITY_OFFSET+BC250_ADAPTER_IDENTITY_BYTES)
struct bc250_adapter_identity {
    unsigned int magic;
    unsigned int version;
    unsigned int size;
    unsigned int luid_low;
    unsigned int luid_high; // Bit pattern of the signed Windows LUID.HighPart.
    unsigned int reserved;
};
typedef char bc250_adapter_identity_size_check[
    sizeof(struct bc250_adapter_identity)==BC250_ADAPTER_IDENTITY_BYTES ? 1 : -1];
