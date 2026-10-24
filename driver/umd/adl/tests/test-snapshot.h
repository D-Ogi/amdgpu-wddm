// The snapshot that the host tests inject into the test build of the ADL subset (Bc250AdlTest_SetSnapshot).
// Used by adl-unit-test.cpp and by ags-client.cpp --inject.
#pragma once

#include <string.h>

#include "../adl-abi.h"
#include "../adl-model.h"

namespace test_snapshot {

inline void Copy(char* dst, size_t cap, const char* src) { strncpy_s(dst, cap, src, _TRUNCATE); }

// Two GPUs. GPU 0 has two sources and three displays (one clone pair on source 0, one rotated display on
// source 1); GPU 1 has no source and gets a placeholder entry.
inline void MakeSnapshot(AdlSnapshot* s)
{
    memset(s, 0, sizeof(*s));
    s->gpu_count = 2;
    AdlGpu* g = &s->gpus[0];
    g->luid_low = 0x1234;
    g->vendor_id = 0x1002;
    g->device_id = 0x13fe;
    g->subsys_id = 0x00011002;
    g->revision_id = 0;
    g->bus = 3;
    g->device = 0;
    g->function = 0;
    g->dedicated_video_bytes = 8ull << 30;
    Copy(g->instance_id, sizeof(g->instance_id), "PCI\\VEN_1002&DEV_13FE&SUBSYS_00011002&REV_00\\4&1A2B3C4D&0&0008");
    Copy(g->name, sizeof(g->name), "Test GPU zero");
    Copy(g->driver_version, sizeof(g->driver_version), "0.7.216.104");
    Copy(g->driver_key, sizeof(g->driver_key), "{4d36e968-e325-11ce-bfc1-08002be10318}\\0001");
    g = &s->gpus[1];
    g->luid_low = 0x5678;
    g->vendor_id = 0x1002;
    g->device_id = 0x73bf;
    g->revision_id = 0xc1;
    g->bus = 10;
    g->device = 0;
    g->function = 0;
    g->dedicated_video_bytes = 16ull << 30;
    Copy(g->instance_id, sizeof(g->instance_id), "PCI\\VEN_1002&DEV_73BF&SUBSYS_0E3A1002&REV_C1\\6&9&0&0010");
    Copy(g->name, sizeof(g->name), "Test GPU one");
    Copy(g->driver_version, sizeof(g->driver_version), "0.7.216.105");
    s->entry_count = 3;
    s->entries[0] = {0, 1, 0, 0, "\\\\.\\DISPLAY1"};
    s->entries[1] = {0, 1, 1, 1, "\\\\.\\DISPLAY2"};
    s->entries[2] = {1, 0, -1, -1, ""};
    s->display_count = 3;
    AdlDisplay* d = &s->displays[0];
    *d = {};
    d->gpu = 0; d->entry = 0; d->index = 0; d->target_id = 0x100;
    d->display_type = ADL_DT_DIGITAL_FLAT_PANEL; d->output_type = ADL_DOT_DIGITAL;
    d->connector = ADL_DISPLAY_CONTYPE_DISPLAYPORT;
    d->x = 0; d->y = 0; d->width = 2560; d->height = 1440; d->refresh_hz = 59.95f;
    Copy(d->name, sizeof(d->name), "Monitor A");
    Copy(d->manufacturer, sizeof(d->manufacturer), "AAA");
    d = &s->displays[1];
    *d = {};
    d->gpu = 0; d->entry = 1; d->index = 1; d->target_id = 0x101;
    d->display_type = ADL_DT_DIGITAL_FLAT_PANEL; d->output_type = ADL_DOT_DIGITAL;
    d->connector = ADL_DISPLAY_CONTYPE_HDMI_TYPE_A;
    d->x = 2560; d->y = 0; d->width = 1080; d->height = 1920; d->refresh_hz = 60.0f; d->orientation = 90;
    Copy(d->name, sizeof(d->name), "Monitor B");
    d = &s->displays[2];
    *d = {};
    d->gpu = 0; d->entry = 0; d->index = 2; d->target_id = 0x102;
    d->display_type = ADL_DT_DIGITAL_FLAT_PANEL; d->output_type = ADL_DOT_DIGITAL;
    d->connector = ADL_DISPLAY_CONTYPE_DISPLAYPORT;
    d->x = 0; d->y = 0; d->width = 2560; d->height = 1440; d->refresh_hz = 59.95f;
    Copy(d->name, sizeof(d->name), "Monitor C (clone of A)");
}

}  // namespace test_snapshot
