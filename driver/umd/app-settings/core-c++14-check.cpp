// SPDX-License-Identifier: MIT
// Compile check of app-settings-core.h with the router DLL's flags: C++14, no /EHsc, /W4 /WX
// (tools/build/build-umd-router.ps1). test-umd-app-settings.ps1 compiles this file with /c; nothing runs it.
#include "app-settings-core.h"

bool core_check_render_on_cpu(const wchar_t* image) {
    namespace as = amdgpu_wddm::app_settings;
    as::Settings settings;
    as::resolve(image, as::system_sources(), settings);
    return as::render_on_cpu(settings) && settings[as::Setting::RenderOnCpu].source != as::Source::None &&
           as::source_name(settings[as::Setting::RenderOnCpu].source)[0] != 0;
}
