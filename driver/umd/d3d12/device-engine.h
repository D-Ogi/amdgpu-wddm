// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
namespace native12 {
struct Device;
class DeviceEngine;
// Creates the actual inline engine device on runtime-backed hosted RADV.
// Failed construction publishes no DeviceEngine; uncertain teardown is retained.
HRESULT create_device_engine(Device&) noexcept;
void destroy_device_engine(Device&) noexcept;
}
