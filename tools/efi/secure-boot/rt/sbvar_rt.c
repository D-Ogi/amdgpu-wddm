/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * bc250-sbvar: the one part of the BC-250 stage that stays in memory as code after ExitBootServices
 * (docs/design/efi-resident-stage.md, "Reported mode"). It is a UEFI runtime driver. The Secure Boot module
 * starts it only when the operator asks for sb.reported=1.
 *
 * It serves a few read-only variables from RAM (struct sb_var_store) in front of the firmware's variable
 * services. It puts its three functions into the runtime services table and keeps the firmware's functions for
 * every other name.
 *
 * What it never does:
 * - It starts no write of its own. SetVariable is called only to pass on a call that the operating system made,
 *   for a name that is not in the store. A write to a name of the store returns EFI_WRITE_PROTECTED.
 * - It keeps nothing after a restart: no NVRAM, no file, no flash.
 *
 * The table conversion at SetVirtualAddressMap is left to the firmware, as edk2 does it
 * (MdeModulePkg/Core/RuntimeDxe/Runtime.c, commit 8628578dacd3: the runtime images are relocated, then every
 * member of the runtime services table is converted and the CRC is set again). This driver converts only the
 * three firmware pointers that it holds. Whether the AMI core of the BC-250 does the same is a lab question.
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "sb_vars.h"

/* The typed form of SetVariable. It is here, and not in efi.h, because this pass-through is the only call of it
 * in the whole stage. */
typedef EFI_STATUS (EFIAPI *set_variable_fn)(CHAR16 *name, EFI_GUID *guid, UINT32 attributes, UINTN size, VOID *data);
typedef EFI_STATUS (EFIAPI *get_variable_fn)(CHAR16 *name, EFI_GUID *guid, UINT32 *attributes, UINTN *size, VOID *data);
typedef EFI_STATUS (EFIAPI *convert_pointer_fn)(UINTN disposition, VOID **address);

static const EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const EFI_GUID virtual_address_change_guid = EFI_EVENT_GROUP_VIRTUAL_ADDRESS_CHANGE;

static struct sb_var_store store;
static get_variable_fn firmware_get;
static sb_vars_next_fn firmware_next;
static set_variable_fn firmware_set;
static convert_pointer_fn convert_pointer;

static EFI_STATUS EFIAPI get_variable(CHAR16 *name, EFI_GUID *guid, UINT32 *attributes, UINTN *size, VOID *data)
{
	int index = (name != NULL && guid != NULL) ? sb_vars_find(&store, name, guid) : -1;
	if (index >= 0) return sb_vars_get(&store, index, attributes, size, data);
	return firmware_get(name, guid, attributes, size, data);
}

static EFI_STATUS EFIAPI get_next_variable_name(UINTN *size, CHAR16 *name, EFI_GUID *guid)
{
	return sb_vars_next(&store, firmware_next, size, name, guid);
}

static EFI_STATUS EFIAPI set_variable(CHAR16 *name, EFI_GUID *guid, UINT32 attributes, UINTN size, VOID *data)
{
	if (name != NULL && guid != NULL && sb_vars_find(&store, name, guid) >= 0) return EFI_WRITE_PROTECTED;
	return firmware_set(name, guid, attributes, size, data);
}

/* SetVirtualAddressMap: the firmware's functions move, so the pointers to them move. */
static VOID EFIAPI on_virtual_address_change(EFI_EVENT event, VOID *context)
{
	(void)event;
	(void)context;
	convert_pointer(0, (VOID **)&firmware_get);
	convert_pointer(0, (VOID **)&firmware_next);
	convert_pointer(0, (VOID **)&firmware_set);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	EFI_BOOT_SERVICES *bs = st->BootServices;
	EFI_RUNTIME_SERVICES *rt = st->RuntimeServices;
	EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
	EFI_EVENT event = NULL;
	UINT32 crc = 0;

	/* The store comes from the stage through the load options: one block of exactly the known size. */
	if (EFI_ERROR(bs->HandleProtocol(image, &loaded_image_guid, (VOID **)&loaded)) || loaded == NULL)
		return EFI_LOAD_ERROR;
	if (loaded->LoadOptions == NULL || loaded->LoadOptionsSize != sizeof(store)) return EFI_INVALID_PARAMETER;
	/* The firmware must have put this image into runtime memory. If it did not, the image goes away at
	 * ExitBootServices, and a pointer to it must not stay in the table. */
	if (loaded->ImageCodeType != EfiRuntimeServicesCode) return EFI_UNSUPPORTED;
	memcpy(&store, loaded->LoadOptions, sizeof(store));
	if (!sb_vars_sound(&store) || store.count == 0) return EFI_INVALID_PARAMETER;
	if (rt->GetVariable == NULL || rt->GetNextVariableName == NULL || rt->SetVariable_NeverCalled == NULL ||
	    rt->ConvertPointer == NULL)
		return EFI_UNSUPPORTED;

	if (EFI_ERROR(bs->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, on_virtual_address_change, NULL,
					&virtual_address_change_guid, &event)))
		return EFI_OUT_OF_RESOURCES;

	firmware_get = rt->GetVariable;
	firmware_next = rt->GetNextVariableName;
	firmware_set = (set_variable_fn)rt->SetVariable_NeverCalled;
	convert_pointer = rt->ConvertPointer;
	rt->GetVariable = get_variable;
	rt->GetNextVariableName = get_next_variable_name;
	rt->SetVariable_NeverCalled = (VOID *)set_variable;
	/* The CRC of a table covers the table with the CRC member as zero. */
	rt->Hdr.CRC32 = 0;
	(void)bs->CalculateCrc32(rt, rt->Hdr.HeaderSize, &crc);
	rt->Hdr.CRC32 = crc;
	return EFI_SUCCESS;
}
