// The part of the AMD Display Library (ADL) binary interface that the open ADL subset implements: return codes,
// the allocation callback, the context handle and the structures of the exported functions.
//
// PROVENANCE: layouts, constants and signatures follow GPUOpen-LibrariesAndSDKs/display-library include/adl_sdk.h,
// adl_defines.h and adl_structures.h @ 84c881cb2136f04287eb3af12de00a9e2867aa9e (MIT, notice in each header).
// Declared here again, only the types this library uses. The sizes are checked below; the AGS 6.3 binary reads
// AdapterInfo, ADLDisplayInfo, ADLDisplayMap and ADLSLSTarget arrays at the same strides (docs/design/adl-subset.md).
#pragma once

#include <stddef.h>

// The exported functions use the default C calling convention of the platform (cdecl on x86); the callback is
// stdcall (adl_sdk.h). The x86 AGS 6.3 binary removes the arguments itself after each ADL call.
#define ADL_API __cdecl

#define ADL_MAX_PATH 256

#define ADL_OK_WARNING 1
#define ADL_OK 0
#define ADL_ERR -1
#define ADL_ERR_NOT_INIT -2
#define ADL_ERR_INVALID_PARAM -3
#define ADL_ERR_INVALID_PARAM_SIZE -4
#define ADL_ERR_INVALID_ADL_IDX -5
#define ADL_ERR_INVALID_CONTROLLER_IDX -6
#define ADL_ERR_INVALID_DIPLAY_IDX -7
#define ADL_ERR_NOT_SUPPORTED -8
#define ADL_ERR_NULL_POINTER -9
#define ADL_ERR_DISABLED_ADAPTER -10
#define ADL_ERR_INVALID_CALLBACK -11

// ADLDisplayInfo.iDisplayType
#define ADL_DT_MONITOR 0
#define ADL_DT_LCD_PANEL 2
#define ADL_DT_DIGITAL_FLAT_PANEL 3

// ADLDisplayInfo.iDisplayOutputType
#define ADL_DOT_UNKNOWN 0
#define ADL_DOT_COMPOSITE 1
#define ADL_DOT_SVIDEO 2
#define ADL_DOT_ANALOG 3
#define ADL_DOT_DIGITAL 4

// ADLDisplayInfo.iDisplayConnector
#define ADL_DISPLAY_CONTYPE_UNKNOWN 0
#define ADL_DISPLAY_CONTYPE_VGA 1
#define ADL_DISPLAY_CONTYPE_DVI_D 2
#define ADL_DISPLAY_CONTYPE_DVI_I 3
#define ADL_DISPLAY_CONTYPE_HDMI_TYPE_A 10
#define ADL_DISPLAY_CONTYPE_SVIDEO 12
#define ADL_DISPLAY_CONTYPE_COMPOSITE 13
#define ADL_DISPLAY_CONTYPE_DISPLAYPORT 15
#define ADL_DISPLAY_CONTYPE_EDP 16
#define ADL_DISPLAY_CONTYPE_WIRELESSDISPLAY 17
#define ADL_DISPLAY_CONTYPE_USB_TYPE_C 18

// ADLDisplayInfo.iDisplayInfoMask and iDisplayInfoValue
#define ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED 0x00000001
#define ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED 0x00000002

// AdapterInfo.iVendorID: ADL writes the AMD vendor as the decimal number 1002, not as 0x1002.
#define ADL_AMD_VENDOR_ID 1002

typedef void*(__stdcall* ADL_MAIN_MALLOC_CALLBACK)(int);
typedef void* ADL_CONTEXT_HANDLE;

typedef enum ADLThreadingModel { ADL_THREADING_UNLOCKED = 0, ADL_THREADING_LOCKED } ADLThreadingModel;

typedef struct AdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[ADL_MAX_PATH];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[ADL_MAX_PATH];
    char strDisplayName[ADL_MAX_PATH];
    int iPresent;
    int iExist;
    char strDriverPath[ADL_MAX_PATH];
    char strDriverPathExt[ADL_MAX_PATH];
    char strPNPString[ADL_MAX_PATH];
    int iOSDisplayIndex;
} AdapterInfo, *LPAdapterInfo;

typedef struct ADLMemoryInfo {
    long long iMemorySize;
    char strMemoryType[ADL_MAX_PATH];
    long long iMemoryBandwidth;
} ADLMemoryInfo, *LPADLMemoryInfo;

typedef struct ADLVersionsInfo {
    char strDriverVer[ADL_MAX_PATH];
    char strCatalystVersion[ADL_MAX_PATH];
    char strCatalystWebLink[ADL_MAX_PATH];
} ADLVersionsInfo, *LPADLVersionsInfo;

typedef struct ADLVersionsInfoX2 {
    char strDriverVer[ADL_MAX_PATH];
    char strCatalystVersion[ADL_MAX_PATH];
    char strCrimsonVersion[ADL_MAX_PATH];
    char strCatalystWebLink[ADL_MAX_PATH];
} ADLVersionsInfoX2, *LPADLVersionsInfoX2;

typedef struct ADLDisplayID {
    int iDisplayLogicalIndex;
    int iDisplayPhysicalIndex;
    int iDisplayLogicalAdapterIndex;
    int iDisplayPhysicalAdapterIndex;
} ADLDisplayID, *LPADLDisplayID;

typedef struct ADLDisplayInfo {
    ADLDisplayID displayID;
    int iDisplayControllerIndex;
    char strDisplayName[ADL_MAX_PATH];
    char strDisplayManufacturerName[ADL_MAX_PATH];
    int iDisplayType;
    int iDisplayOutputType;
    int iDisplayConnector;
    int iDisplayInfoMask;
    int iDisplayInfoValue;
} ADLDisplayInfo, *LPADLDisplayInfo;

typedef struct ADLMode {
    int iAdapterIndex;
    ADLDisplayID displayID;
    int iXPos;
    int iYPos;
    int iXRes;
    int iYRes;
    int iColourDepth;
    float fRefreshRate;
    int iOrientation;
    int iModeFlag;
    int iModeMask;
    int iModeValue;
} ADLMode, *LPADLMode;

typedef struct ADLDisplayMap {
    int iDisplayMapIndex;
    ADLMode displayMode;
    int iNumDisplayTarget;
    int iFirstDisplayTargetArrayIndex;
    int iDisplayMapMask;
    int iDisplayMapValue;
} ADLDisplayMap, *LPADLDisplayMap;

typedef struct ADLDisplayTarget {
    ADLDisplayID displayID;
    int iDisplayMapIndex;
    int iDisplayTargetMask;
    int iDisplayTargetValue;
} ADLDisplayTarget, *LPADLDisplayTarget;

typedef struct ADLSLSGrid {
    int iAdapterIndex;
    int iSLSGridIndex;
    int iSLSGridRow;
    int iSLSGridColumn;
    int iSLSGridMask;
    int iSLSGridValue;
} ADLSLSGrid, *LPADLSLSGrid;

typedef struct ADLSLSMap {
    int iAdapterIndex;
    int iSLSMapIndex;
    ADLSLSGrid grid;
    int iSurfaceMapIndex;
    int iOrientation;
    int iNumSLSTarget;
    int iFirstSLSTargetArrayIndex;
    int iNumNativeMode;
    int iFirstNativeModeArrayIndex;
    int iNumBezelMode;
    int iFirstBezelModeArrayIndex;
    int iNumBezelOffset;
    int iFirstBezelOffsetArrayIndex;
    int iSLSMapMask;
    int iSLSMapValue;
} ADLSLSMap, *LPADLSLSMap;

typedef struct ADLSLSTarget {
    int iAdapterIndex;
    int iSLSMapIndex;
    ADLDisplayTarget displayTarget;
    int iSLSGridPositionX;
    int iSLSGridPositionY;
    ADLMode viewSize;
    int iSLSTargetMask;
    int iSLSTargetValue;
} ADLSLSTarget, *LPADLSLSTarget;

typedef struct ADLSLSMode {
    int iAdapterIndex;
    int iSLSMapIndex;
    int iSLSModeIndex;
    ADLMode displayMode;
    int iSLSNativeModeMask;
    int iSLSNativeModeValue;
} ADLSLSMode, *LPADLSLSMode;

typedef struct ADLBezelTransientMode {
    int iAdapterIndex;
    int iSLSMapIndex;
    int iSLSModeIndex;
    ADLMode displayMode;
    int iNumBezelOffset;
    int iFirstBezelOffsetArrayIndex;
    int iSLSBezelTransientModeMask;
    int iSLSBezelTransientModeValue;
} ADLBezelTransientMode, *LPADLBezelTransientMode;

typedef struct ADLSLSOffset {
    int iAdapterIndex;
    int iSLSMapIndex;
    ADLDisplayID displayID;
    int iBezelModeIndex;
    int iBezelOffsetX;
    int iBezelOffsetY;
    int iDisplayWidth;
    int iDisplayHeight;
    int iBezelOffsetMask;
    int iBezelffsetValue;
} ADLSLSOffset, *LPADLSLSOffset;

// Strides that the AGS 6.3 binary uses (0x624, 0x228, 0x50, 0x70, 0x110, 0x400) and the header sizes of the rest.
// No pointer member, so x86 and x64 agree.
static_assert(sizeof(AdapterInfo) == 0x624, "AdapterInfo");
static_assert(sizeof(ADLMemoryInfo) == 0x110, "ADLMemoryInfo");
static_assert(sizeof(ADLVersionsInfo) == 0x300, "ADLVersionsInfo");
static_assert(sizeof(ADLVersionsInfoX2) == 0x400, "ADLVersionsInfoX2");
static_assert(sizeof(ADLDisplayInfo) == 0x228, "ADLDisplayInfo");
static_assert(sizeof(ADLMode) == 60, "ADLMode");
static_assert(sizeof(ADLDisplayMap) == 0x50, "ADLDisplayMap");
static_assert(sizeof(ADLDisplayTarget) == 28, "ADLDisplayTarget");
static_assert(sizeof(ADLSLSMap) == 80, "ADLSLSMap");
static_assert(sizeof(ADLSLSTarget) == 0x70, "ADLSLSTarget");
static_assert(sizeof(ADLSLSMode) == 80, "ADLSLSMode");
static_assert(sizeof(ADLBezelTransientMode) == 88, "ADLBezelTransientMode");
static_assert(sizeof(ADLSLSOffset) == 52, "ADLSLSOffset");
static_assert(offsetof(AdapterInfo, iExist) == 0x31c, "AdapterInfo.iExist");
static_assert(offsetof(AdapterInfo, strDisplayName) == 0x218, "AdapterInfo.strDisplayName");
static_assert(offsetof(ADLDisplayInfo, iDisplayInfoValue) == 0x224, "ADLDisplayInfo.iDisplayInfoValue");
static_assert(offsetof(ADLMemoryInfo, iMemoryBandwidth) == 0x108, "ADLMemoryInfo.iMemoryBandwidth");
