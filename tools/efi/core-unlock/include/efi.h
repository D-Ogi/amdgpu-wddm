/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The subset of the UEFI interfaces that core-unlock uses, written from the UEFI Specification 2.10 (system
 * table, boot and runtime services, Loaded Image, Device Path, Simple File System, File, Simple Text, PCI I/O,
 * Graphics Output) and the PI Specification 1.8 volume 2 (MP Services). No code is copied from an implementation.
 *
 * Every table lists all of its members in the order of the specification, so the offsets of the members we call
 * are right. A member we never call is an opaque pointer. SetVariable is one of them on purpose: this application
 * writes no NVRAM variable (docs/design/efi-core-unlock.md), and its header gives no typed way to call it.
 */
#ifndef CU_EFI_H
#define CU_EFI_H

#define EFIAPI __attribute__((ms_abi))

typedef unsigned long long UINT64;
typedef long long INT64;
typedef unsigned int UINT32;
typedef int INT32;
typedef unsigned short UINT16;
typedef unsigned char UINT8;
typedef UINT64 UINTN;
typedef INT64 INTN;
typedef unsigned short CHAR16;
typedef char CHAR8;
typedef UINT8 BOOLEAN;
typedef void VOID;
typedef UINTN EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;
typedef UINT64 EFI_PHYSICAL_ADDRESS;

typedef struct { UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8]; } EFI_GUID;

#define EFI_ERROR_BIT		(1ull << 63)
#define EFI_ERROR(s)		(((s) & EFI_ERROR_BIT) != 0)
#define EFI_SUCCESS		0ull
#define EFI_LOAD_ERROR		(EFI_ERROR_BIT | 1ull)
#define EFI_INVALID_PARAMETER	(EFI_ERROR_BIT | 2ull)
#define EFI_UNSUPPORTED		(EFI_ERROR_BIT | 3ull)
#define EFI_BUFFER_TOO_SMALL	(EFI_ERROR_BIT | 5ull)
#define EFI_NOT_READY		(EFI_ERROR_BIT | 6ull)
#define EFI_DEVICE_ERROR	(EFI_ERROR_BIT | 7ull)
#define EFI_NOT_FOUND		(EFI_ERROR_BIT | 14ull)
#define EFI_ABORTED		(EFI_ERROR_BIT | 21ull)

typedef struct { UINT64 Signature; UINT32 Revision; UINT32 HeaderSize; UINT32 CRC32; UINT32 Reserved; } EFI_TABLE_HEADER;

/* ---- text console ---------------------------------------------------------------------------- */

typedef struct { UINT16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
	EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
	EFI_EVENT WaitForKey;
};

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	VOID *Reset;
	EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, const CHAR16 *String);
	VOID *TestString;
	VOID *QueryMode;
	VOID *SetMode;
	VOID *SetAttribute;
	VOID *ClearScreen;
	VOID *SetCursorPosition;
	VOID *EnableCursor;
	VOID *Mode;
};

/* ---- device paths ---------------------------------------------------------------------------- */

typedef struct { UINT8 Type; UINT8 SubType; UINT8 Length[2]; } EFI_DEVICE_PATH_PROTOCOL;
#define EFI_DP_TYPE_MEDIA		0x04u
#define EFI_DP_MEDIA_FILEPATH		0x04u
#define EFI_DP_TYPE_END			0x7Fu
#define EFI_DP_END_ENTIRE		0xFFu

/* ---- runtime services ------------------------------------------------------------------------ */

typedef enum { EfiResetCold = 0, EfiResetWarm = 1, EfiResetShutdown = 2, EfiResetPlatformSpecific = 3 } EFI_RESET_TYPE;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	VOID *GetTime;
	VOID *SetTime;
	VOID *GetWakeupTime;
	VOID *SetWakeupTime;
	VOID *SetVirtualAddressMap;
	VOID *ConvertPointer;
	VOID *GetVariable;
	VOID *GetNextVariableName;
	VOID *SetVariable_NeverCalled;	/* no NVRAM write: see the head of this file */
	VOID *GetNextHighMonotonicCount;
	VOID (EFIAPI *ResetSystem)(EFI_RESET_TYPE ResetType, EFI_STATUS ResetStatus, UINTN DataSize, VOID *ResetData);
	VOID *UpdateCapsule;
	VOID *QueryCapsuleCapabilities;
	VOID *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

/* ---- boot services --------------------------------------------------------------------------- */

typedef enum { EfiReservedMemoryType = 0, EfiLoaderCode = 1, EfiLoaderData = 2 } EFI_MEMORY_TYPE;
typedef enum { AllHandles = 0, ByRegisterNotify = 1, ByProtocol = 2 } EFI_LOCATE_SEARCH_TYPE;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	VOID *RaiseTPL;
	VOID *RestoreTPL;
	VOID *AllocatePages;
	VOID *FreePages;
	VOID *GetMemoryMap;
	EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
	EFI_STATUS (EFIAPI *FreePool)(VOID *Buffer);
	VOID *CreateEvent;
	VOID *SetTimer;
	VOID *WaitForEvent;
	VOID *SignalEvent;
	VOID *CloseEvent;
	VOID *CheckEvent;
	VOID *InstallProtocolInterface;
	VOID *ReinstallProtocolInterface;
	VOID *UninstallProtocolInterface;
	EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, const EFI_GUID *Protocol, VOID **Interface);
	VOID *Reserved;
	VOID *RegisterProtocolNotify;
	VOID *LocateHandle;
	VOID *LocateDevicePath;
	VOID *InstallConfigurationTable;
	EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle,
				       EFI_DEVICE_PATH_PROTOCOL *DevicePath, VOID *SourceBuffer, UINTN SourceSize,
				       EFI_HANDLE *ImageHandle);
	EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
	VOID *Exit;
	VOID *UnloadImage;
	VOID *ExitBootServices;
	VOID *GetNextMonotonicCount;
	EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
	VOID *SetWatchdogTimer;
	VOID *ConnectController;
	VOID *DisconnectController;
	VOID *OpenProtocol;
	VOID *CloseProtocol;
	VOID *OpenProtocolInformation;
	VOID *ProtocolsPerHandle;
	EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType, const EFI_GUID *Protocol,
						VOID *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buffer);
	EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
	VOID *InstallMultipleProtocolInterfaces;
	VOID *UninstallMultipleProtocolInterfaces;
	VOID *CalculateCrc32;
	VOID *CopyMem;
	VOID *SetMem;
	VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct { EFI_GUID VendorGuid; VOID *VendorTable; } EFI_CONFIGURATION_TABLE;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	EFI_RUNTIME_SERVICES *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Loaded Image ---------------------------------------------------------------------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
	{ 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
typedef struct {
	UINT32 Revision;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	EFI_DEVICE_PATH_PROTOCOL *FilePath;
	VOID *Reserved;
	UINT32 LoadOptionsSize;
	VOID *LoadOptions;
	VOID *ImageBase;
	UINT64 ImageSize;
	EFI_MEMORY_TYPE ImageCodeType;
	EFI_MEMORY_TYPE ImageDataType;
	VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
	{ 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

/* ---- files ----------------------------------------------------------------------------------- */

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
	{ 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_FILE_MODE_READ	0x0000000000000001ull
#define EFI_FILE_MODE_WRITE	0x0000000000000002ull
#define EFI_FILE_MODE_CREATE	0x8000000000000000ull

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, const CHAR16 *FileName,
				  UINT64 OpenMode, UINT64 Attributes);
	EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
	EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);	/* closes the handle; a warning if not deleted */
	EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
	VOID *GetPosition;
	VOID *SetPosition;
	VOID *GetInfo;
	VOID *SetInfo;
	EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
};

/* ---- PCI I/O --------------------------------------------------------------------------------- */

#define EFI_PCI_IO_PROTOCOL_GUID \
	{ 0x4CF5B200, 0x68B8, 0x4CA5, { 0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A } }
typedef enum { EfiPciIoWidthUint8 = 0, EfiPciIoWidthUint16 = 1, EfiPciIoWidthUint32 = 2 } EFI_PCI_IO_PROTOCOL_WIDTH;

typedef struct EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
typedef struct {
	EFI_STATUS (EFIAPI *Read)(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT8 BarIndex,
				  UINT64 Offset, UINTN Count, VOID *Buffer);
	EFI_STATUS (EFIAPI *Write)(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT8 BarIndex,
				   UINT64 Offset, UINTN Count, VOID *Buffer);
} EFI_PCI_IO_PROTOCOL_ACCESS;
typedef struct {
	EFI_STATUS (EFIAPI *Read)(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset,
				  UINTN Count, VOID *Buffer);
	VOID *Write;
} EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS;
struct EFI_PCI_IO_PROTOCOL {
	VOID *PollMem;
	VOID *PollIo;
	EFI_PCI_IO_PROTOCOL_ACCESS Mem;
	EFI_PCI_IO_PROTOCOL_ACCESS Io;
	EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS Pci;
	VOID *CopyMem;
	VOID *Map;
	VOID *Unmap;
	VOID *AllocateBuffer;
	VOID *FreeBuffer;
	VOID *Flush;
	EFI_STATUS (EFIAPI *GetLocation)(EFI_PCI_IO_PROTOCOL *This, UINTN *Segment, UINTN *Bus, UINTN *Device,
					 UINTN *Function);
	VOID *Attributes;
	EFI_STATUS (EFIAPI *GetBarAttributes)(EFI_PCI_IO_PROTOCOL *This, UINT8 BarIndex, UINT64 *Supports,
					      VOID **Resources);
	VOID *SetBarAttributes;
	UINT64 RomSize;
	VOID *RomImage;
};

/* The ACPI QWORD address space descriptor that GetBarAttributes returns (ACPI 6.5 section 6.4.3.5.1). */
#define CU_ACPI_QWORD_DESCRIPTOR	0x8Au
#define CU_ACPI_RESOURCE_MEMORY		0x00u
typedef struct __attribute__((packed)) {
	UINT8 Desc;
	UINT16 Len;
	UINT8 ResType;
	UINT8 GenFlag;
	UINT8 SpecificFlag;
	UINT64 AddrSpaceGranularity;
	UINT64 AddrRangeMin;
	UINT64 AddrRangeMax;
	UINT64 AddrTranslationOffset;
	UINT64 AddrLen;
} CU_ACPI_QWORD_ADDRESS_DESCRIPTOR;

/* ---- MP Services (PI Specification volume 2) ------------------------------------------------- */

#define EFI_MP_SERVICES_PROTOCOL_GUID \
	{ 0x3FDDA605, 0xA76E, 0x4F46, { 0xAD, 0x29, 0x12, 0xF4, 0x53, 0x1B, 0x3D, 0x08 } }
typedef struct EFI_MP_SERVICES_PROTOCOL EFI_MP_SERVICES_PROTOCOL;
typedef VOID (EFIAPI *EFI_AP_PROCEDURE)(VOID *ProcedureArgument);
typedef struct { UINT32 Package; UINT32 Core; UINT32 Thread; } EFI_CPU_PHYSICAL_LOCATION;
/* PI 1.7 added ExtendedInformation, which the firmware fills only on request. It is here so that a firmware that
 * writes it anyway writes inside the buffer. */
typedef struct { UINT32 Package; UINT32 Module; UINT32 Tile; UINT32 Die; UINT32 Core; UINT32 Thread; }
	EFI_CPU_PHYSICAL_LOCATION2;
typedef struct {
	UINT64 ProcessorId;
	UINT32 StatusFlag;
	EFI_CPU_PHYSICAL_LOCATION Location;
	EFI_CPU_PHYSICAL_LOCATION2 ExtendedInformation;
} EFI_PROCESSOR_INFORMATION;
#define PROCESSOR_AS_BSP_BIT		0x00000001u
#define PROCESSOR_ENABLED_BIT		0x00000002u
#define PROCESSOR_HEALTH_STATUS_BIT	0x00000004u
struct EFI_MP_SERVICES_PROTOCOL {
	EFI_STATUS (EFIAPI *GetNumberOfProcessors)(EFI_MP_SERVICES_PROTOCOL *This, UINTN *NumberOfProcessors,
						   UINTN *NumberOfEnabledProcessors);
	EFI_STATUS (EFIAPI *GetProcessorInfo)(EFI_MP_SERVICES_PROTOCOL *This, UINTN ProcessorNumber,
					      EFI_PROCESSOR_INFORMATION *ProcessorInfoBuffer);
	VOID *StartupAllAPs;
	EFI_STATUS (EFIAPI *StartupThisAP)(EFI_MP_SERVICES_PROTOCOL *This, EFI_AP_PROCEDURE Procedure,
					   UINTN ProcessorNumber, EFI_EVENT WaitEvent, UINTN TimeoutInMicroseconds,
					   VOID *ProcedureArgument, BOOLEAN *Finished);
	VOID *SwitchBSP;
	VOID *EnableDisableAP;
	EFI_STATUS (EFIAPI *WhoAmI)(EFI_MP_SERVICES_PROTOCOL *This, UINTN *ProcessorNumber);
};

/* ---- Graphics Output (UEFI Specification 2.10 section 12.9) --------------------------------------------- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
	{ 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } }
typedef enum {
	PixelRedGreenBlueReserved8BitPerColor = 0, PixelBlueGreenRedReserved8BitPerColor = 1, PixelBitMask = 2,
	PixelBltOnly = 3, PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;
typedef struct { UINT32 RedMask; UINT32 GreenMask; UINT32 BlueMask; UINT32 ReservedMask; } EFI_PIXEL_BITMASK;
typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber, UINTN *SizeOfInfo,
				       EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
	VOID *SetMode_NeverCalled;	/* the report changes no mode */
	VOID *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* ---- configuration tables -------------------------------------------------------------------- */

#define EFI_ACPI_20_TABLE_GUID \
	{ 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
	{ 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }
#define EFI_SMBIOS_TABLE_GUID \
	{ 0xEB9D2D31, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }
#define EFI_SMBIOS3_TABLE_GUID \
	{ 0xF2FD1544, 0x9794, 0x4A2C, { 0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94 } }

#endif
