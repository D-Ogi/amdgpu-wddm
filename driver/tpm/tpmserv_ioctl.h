/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The interface between the kernel filter tpmserv.sys and the user-mode TPM core service. The filter owns the CRB
 * page; the service owns the TPM state and the cryptography. The filter hands out each command once through
 * GET_COMMAND and takes its answer through PUT_RESPONSE.
 *
 * The control device lives from DriverEntry to driver unload, not with the TPM device node: a restart of the TPM
 * device node keeps the handle of the service valid, and a command that was out when the service closed its handle
 * goes out again to the next GET_COMMAND. When no service is connected for 2 s, the filter answers the command with
 * TPM_RC_FAILURE itself (a shutdown ends the service before tpm.sys sends its last commands).
 *
 * Only the LocalSystem account and the Administrators group can open the device (SDDL in tpmserv.c).
 */
#ifndef BC250_TPMSERV_IOCTL_H
#define BC250_TPMSERV_IOCTL_H

#define TPMSERV_DEVICE_NAME	L"\\Device\\Bc250TpmServicer"
#define TPMSERV_SYMLINK_NAME	L"\\DosDevices\\Bc250TpmServicer"
#define TPMSERV_USER_PATH	L"\\\\.\\Bc250TpmServicer"

#define TPMSERV_DEVICE_TYPE	0x8BC2u

/* Output: the command bytes (at most TPMSERV_MAX_COMMAND). Pends until a command is there. */
#define IOCTL_TPMSERV_GET_COMMAND \
	CTL_CODE(TPMSERV_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
/* Input: the response bytes of the command that GET_COMMAND gave. STATUS_INVALID_DEVICE_STATE if no command is out. */
#define IOCTL_TPMSERV_PUT_RESPONSE \
	CTL_CODE(TPMSERV_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
/* Output: struct tpmserv_status. Never pends. */
#define IOCTL_TPMSERV_GET_STATUS \
	CTL_CODE(TPMSERV_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Output: the handoff record of the pre-OS TPM (handoff.h), at most TPM_HANDOFF_MAX_BYTES. The filter gives it once:
 * after a successful call it zeros and frees its copy. STATUS_NOT_FOUND when there is none (status.handoff says
 * why), STATUS_BUFFER_TOO_SMALL when the output buffer is smaller than the record. */
#define IOCTL_TPMSERV_GET_HANDOFF \
	CTL_CODE(TPMSERV_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define TPMSERV_MAX_COMMAND	0xF80u	/* the CRB data buffer; see crb.h */

/* status.handoff */
#define TPMSERV_HANDOFF_PENDING	0u	/* the TPM device has not started yet: the filter has not looked */
#define TPMSERV_HANDOFF_NONE	1u	/* Device (TPM0) has no BCHO, or its range holds no record: no pre-OS TPM */
#define TPMSERV_HANDOFF_READY	2u	/* a record with a sound frame waits for GET_HANDOFF */
#define TPMSERV_HANDOFF_TAKEN	3u	/* the service took the record */
#define TPMSERV_HANDOFF_BAD	4u	/* BCHO names a range, and the range holds no sound record */

#define TPMSERV_STATUS_VERSION	3u
struct tpmserv_status {
	unsigned int version;		/* TPMSERV_STATUS_VERSION */
	unsigned int attached;		/* 1 while the filter has a started TPM device with a mapped page */
	unsigned long long page;	/* the physical address of the CRB page, 0 when not attached */
	unsigned int commands, bad_size, start_while_idle, cancels;
	unsigned int ready, idle, locality_requests, locality_relinquish;
	unsigned int redelivered;	/* commands given out again after the service closed its handle */
	unsigned int failed;		/* commands answered with TPM_RC_FAILURE because no service was connected */
	unsigned int handoff;		/* TPMSERV_HANDOFF_* */
	unsigned int handoff_bytes;	/* the size of the record while it is READY or after it was TAKEN */
};

#endif
