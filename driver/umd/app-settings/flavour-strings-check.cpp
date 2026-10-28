// SPDX-License-Identifier: MIT
// Compile check of the flavour switch of log_once (app-settings.h): test-umd-app-settings.ps1 compiles this file
// with /c /Od, once without and once with AMDGPU_WDDM_LAB_FLAVOUR, and reads the object files. The release object
// must not hold the lab lines of dxvk-profile.h, the lab object must hold them. /Od keeps every function that is
// instantiated, so a lab line that only an optimiser removes still fails here. Nothing runs this code.
#include "app-settings.h"

void flavour_strings_check_log(const amdgpu_wddm::app_settings::Settings& settings) {
    amdgpu_wddm::app_settings::log_once(settings,"d3d12",0u,[](const char* line) {(void)line;});
}
