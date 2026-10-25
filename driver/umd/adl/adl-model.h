// The facts that the open ADL subset reports: one snapshot of the adapters that run our kernel-mode driver, their
// display sources and their active displays. adl-discover.cpp fills it from the operating system; the exported
// functions (adl-main.cpp) only translate it into ADL structures. The test build can replace the discovery with a
// snapshot of its own (BC250_ADL_TEST_HOOKS); the shipped DLL cannot.
#pragma once

#include <stdint.h>

#define ADL_MODEL_MAX_GPUS 16
#define ADL_MODEL_MAX_ENTRIES 64
#define ADL_MODEL_MAX_DISPLAYS 64
#define ADL_MODEL_TEXT 256

// One physical adapter that runs our kernel-mode driver.
struct AdlGpu {
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t vendor_id;   // PCI ids, from the PnP hardware id of the device
    uint32_t device_id;
    uint32_t subsys_id;
    uint32_t revision_id;
    uint32_t bus;         // PCI location, from SetupAPI and D3DKMT (the two must agree)
    uint32_t device;
    uint32_t function;
    uint64_t dedicated_video_bytes;  // D3DKMT_SEGMENTSIZEINFO of the adapter
    uint64_t dedicated_system_bytes;
    uint64_t shared_system_bytes;
    char instance_id[ADL_MODEL_TEXT];     // PnP device instance id, for example PCI\VEN_1002&DEV_....\4&...
    char name[ADL_MODEL_TEXT];            // friendly name or device description of the device node
    char driver_version[ADL_MODEL_TEXT];  // DriverVersion of the device's driver key
    char driver_key[ADL_MODEL_TEXT];      // the driver key name, {class guid}\nnnn
};

// One ADL adapter: one display source (\\.\DISPLAYn) of a GPU, or one placeholder for a GPU without a source.
struct AdlEntry {
    int32_t gpu;             // index into AdlSnapshot.gpus
    int32_t present;         // 1 when the source is attached to the desktop
    int32_t os_display_index;  // EnumDisplayDevices index of the source, -1 for the placeholder
    int32_t source_id;       // VidPN source id, -1 for the placeholder
    char gdi_name[32];       // \\.\DISPLAYn, empty for the placeholder
};

// One active display path (a monitor that shows part of the desktop).
struct AdlDisplay {
    int32_t gpu;
    int32_t entry;           // the ADL adapter (source) that drives it
    int32_t index;           // display index, unique per GPU
    int32_t target_id;       // VidPN target id
    int32_t display_type;    // ADL_DT_*
    int32_t output_type;     // ADL_DOT_*
    int32_t connector;       // ADL_DISPLAY_CONTYPE_*
    int32_t x, y, width, height;
    int32_t orientation;     // 0, 90, 180 or 270
    float refresh_hz;
    char name[ADL_MODEL_TEXT];
    char manufacturer[ADL_MODEL_TEXT];
};

struct AdlSnapshot {
    int32_t gpu_count;
    int32_t entry_count;
    int32_t display_count;
    AdlGpu gpus[ADL_MODEL_MAX_GPUS];
    AdlEntry entries[ADL_MODEL_MAX_ENTRIES];
    AdlDisplay displays[ADL_MODEL_MAX_DISPLAYS];
};

// Fills *out from the operating system. Never fails: no adapter of ours gives an empty snapshot.
void AdlDiscover(AdlSnapshot* out);
