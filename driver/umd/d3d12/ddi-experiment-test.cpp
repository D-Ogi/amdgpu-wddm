// SPDX-License-Identifier: MIT
// The experiment list and its sources (ddi-trace.h): the environment variable, the application profile in
// HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<image>, the machine-wide value under
// HKLM\SOFTWARE\amdgpu-wddm\D3D12, the "none" value, the refusals and the "-off" form every driver default is
// read through. The registry part is read only: this test creates no key, so its own image has no profile, the
// machine-wide value does not exist on a build machine, and both resolve to None without the variable.
#include "ddi-trace.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using native12::DdiExperimentSource;
namespace detail = native12::ddi_detail;

static const char kVariable[] = "AMDGPU_WDDM_D3D12_EXPERIMENT";

static native12::DdiExperimentValue with_variable(const char* value) {
    assert(SetEnvironmentVariableA(kVariable, value));
    return detail::resolve_experiment();
}

int main() {
    // Syntax: lower-case letters, digits, hyphens and commas only.
    assert(detail::experiment_syntax(""));
    assert(detail::experiment_syntax("present-cached,raytracing-tier,a1"));
    assert(!detail::experiment_syntax("Present-cached"));
    assert(!detail::experiment_syntax("a b"));
    assert(!detail::experiment_syntax("a;b"));

    // Matching names whole entries only.
    assert(native12::ddi_experiment_listed("a,bb,c", "bb"));
    assert(native12::ddi_experiment_listed("a,bb,c", "c"));
    assert(native12::ddi_experiment_listed("a,bb,c", "a"));
    assert(!native12::ddi_experiment_listed("a,bb,c", "b"));
    assert(!native12::ddi_experiment_listed("a,bb,c", "bb,c"));
    assert(!native12::ddi_experiment_listed("a,bb,c", ""));
    assert(!native12::ddi_experiment_listed("", "a"));
    assert(!native12::ddi_experiment_listed(nullptr, "a"));

    // M15.14: the increment-2 token carries a geometry, and the parser still reads it exactly, because
    // increment 3 counts it as an explicit on (scanout-mode.h). The parser itself stays strict: a token
    // that is not a geometry is no geometry. The off switch is no geometry either.
    unsigned width = 0, height = 0;
    width = height = 7;
    assert(!native12::ddi_experiment_scanout("scanout-flip-off", &width, &height));
    assert(width == 7 && height == 7);
    assert(detail::experiment_syntax("scanout-flip-off"));
    width = height = 0;
    assert(native12::ddi_experiment_scanout("scanout-flip-1920x1200", &width, &height));
    assert(width == 1920 && height == 1200);
    width = height = 0;
    assert(native12::ddi_experiment_scanout("raytracing-tier,scanout-flip-1280x720,present-cached", &width, &height));
    assert(width == 1280 && height == 720);
    for (const char* refused : {"scanout-flip", "scanout-flip-", "scanout-flip-1920", "scanout-flip-1920x",
                                "scanout-flip-x1200", "scanout-flip-0x1200", "scanout-flip-1920x0",
                                "scanout-flip-1920x1200x2", "scanout-flip-1920-1200", "scanout-flip-123456x1200",
                                "scanout-flip-1920x1200-", "ascanout-flip-1920x1200", "", "a,b"}) {
        width = height = 7;
        assert(!native12::ddi_experiment_scanout(refused, &width, &height));
        assert(width == 7 && height == 7);   // a refusal writes neither output
    }
    assert(!native12::ddi_experiment_scanout(nullptr, &width, &height));
    assert(!native12::ddi_experiment_scanout("scanout-flip-1920x1200", nullptr, &height));
    assert(!native12::ddi_experiment_scanout("scanout-flip-1920x1200", &width, nullptr));
    // The token is also syntax the experiment reader accepts, so it can actually be set.
    assert(detail::experiment_syntax("scanout-flip-1920x1200"));

    // The profile key: the image file name after the last separator, either kind.
    wchar_t key[128]{};
    assert(detail::application_profile_key(L"C:\\Games\\bin\\x64_dx12\\witcher3.exe", key, 128));
    assert(!std::wcscmp(key, L"SOFTWARE\\amdgpu-wddm\\D3D12\\Applications\\witcher3.exe"));
    assert(detail::application_profile_key(L"C:/Games/witcher3.exe", key, 128));
    assert(!std::wcscmp(key, L"SOFTWARE\\amdgpu-wddm\\D3D12\\Applications\\witcher3.exe"));
    assert(detail::application_profile_key(L"witcher3.exe", key, 128));
    assert(!detail::application_profile_key(L"C:\\Games\\", key, 128));
    assert(!detail::application_profile_key(L"C:\\Games\\witcher3.exe", key, 40));

    // The variable wins whenever it is present.
    native12::DdiExperimentValue v = with_variable("raytracing-tier,present-cached");
    assert(v.source == DdiExperimentSource::Environment && !std::strcmp(v.text, "raytracing-tier,present-cached"));
    v = with_variable("none");
    assert(v.source == DdiExperimentSource::Environment && !v.text[0]);
    v = with_variable("");
    assert(v.source == DdiExperimentSource::Environment && !v.text[0]);
    v = with_variable("Raytracing-Tier");
    assert(v.source == DdiExperimentSource::Invalid && !v.text[0]);
    const std::string long_list(300, 'a');
    v = with_variable(long_list.c_str());
    assert(v.source == DdiExperimentSource::Invalid && !v.text[0]);
    const std::string longest(255, 'a');
    v = with_variable(longest.c_str());
    assert(v.source == DdiExperimentSource::Environment && std::strlen(v.text) == 255);

    // The "-off" form: every driver default is read through it, and it matches whole entries only.
    v = with_variable("deferred-replay-off");
    assert(v.source == DdiExperimentSource::Environment);
    assert(native12::ddi_experiment_off("deferred-replay"));
    assert(!native12::ddi_experiment_off("deferred-repla"));
    assert(!native12::ddi_experiment_off("replay"));
    assert(!native12::ddi_experiment_off(""));
    assert(!native12::ddi_experiment_off(nullptr));
    // The positive name of a default is accepted and turns nothing off.
    assert(!native12::ddi_experiment_listed("raytracing-tier,recording-bind", "raytracing-tier-off"));
    // The shader model switches (adapter-caps.cpp): valid syntax, matched whole, neither one implies the other.
    assert(detail::experiment_syntax("shader-model-67-off,shader-model-68-off"));
    assert(native12::ddi_experiment_listed("present-cached,shader-model-68-off", "shader-model-68-off"));
    assert(!native12::ddi_experiment_listed("shader-model-68-off", "shader-model-67-off"));
    assert(!native12::ddi_experiment_listed("shader-model-67", "shader-model-67-off"));
    // A name too long for the "-off" buffer is no switch, never an accidental off.
    assert(!native12::ddi_experiment_off(std::string(60, 'a').c_str()));

    // Without the variable: this image has no profile and the build machine has no machine-wide value.
    assert(SetEnvironmentVariableA(kVariable, nullptr));
    v = detail::resolve_experiment();
    assert(v.source == DdiExperimentSource::None && !v.text[0]);
    char text[256]{};
    assert(detail::application_profile(text, sizeof(text)) == 0 && !text[0]);
    assert(detail::machine_experiment(text, sizeof(text)) == 0 && !text[0]);

    // The first device removal of a process names its own site, once, with no switch set
    // (ddi_first_removal). What is observable here is the always-on refusal budget: one line for the
    // first removal and nothing for any later one, so a cascade cannot bury the decision that
    // removed the device. BD-105 round 3 is why this line exists at all: the Vulkan WSI read the
    // removed-device sentinel from a shared fence of this driver's D3D12 device and no log of the
    // process said that a removal had happened.
    assert(!std::strcmp(native12::ddi_source_name("a\\b\\queue-engine.cpp"), "queue-engine.cpp"));
    assert(!std::strcmp(native12::ddi_source_name("a/b/native-queue-ddi.cpp"), "native-queue-ddi.cpp"));
    assert(!std::strcmp(native12::ddi_source_name("device-state.h"), "device-state.h"));
    assert(!std::strcmp(native12::ddi_source_name(""), ""));
    assert(!std::strcmp(native12::ddi_source_name(nullptr), "?"));
    const int32_t budget_before = native12::ddi_refusal_budget.load();
    native12::ddi_first_removal("device-remove", __FILE__, 1);
    const int32_t budget_after_first = native12::ddi_refusal_budget.load();
    assert(budget_before - budget_after_first == 1);
    native12::ddi_first_removal("hosted-remove-device", __FILE__, 2);
    native12::ddi_first_removal("device-remove", __FILE__, 3);
    assert(native12::ddi_refusal_budget.load() == budget_after_first);

    std::puts("PASS ddi experiment sources: syntax, matching, profile key, machine-wide value, variable "
              "precedence, none, refusals, the -off form of every default, and the one-shot first-removal line");
    return 0;
}
