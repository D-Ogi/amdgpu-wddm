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

    // The first failing DDI of each group of the Vulkan WSI's DXGI present route, named with its
    // HRESULT and its site, on the always-on channel (ddi_first_failure). Round 4 of BD-105 named
    // only the first device REMOVAL, and the audit of 2026-10-10 is right that a removal is a
    // consequence: the question left open is which call of the route refused first, and a removal
    // reason read at the end of a two-second timeout cannot answer it.
    //
    // The classification is pure, so every boundary of the route's call table is driven here.
    using native12::DdiFailureGroup;
    assert(native12::ddi_failure_group("pfnCloseCommandList") == DdiFailureGroup::List);
    assert(native12::ddi_failure_group("pfnCloseCommandList_0040") == DdiFailureGroup::List);
    assert(native12::ddi_failure_group("pfnExecuteCommandLists") == DdiFailureGroup::Queue);
    assert(native12::ddi_failure_group("pfnCreateCommandQueue") == DdiFailureGroup::Queue);
    assert(native12::ddi_failure_group("pfnSignalSynchronizationObject") == DdiFailureGroup::Queue);
    assert(native12::ddi_failure_group("pfnWaitForSynchronizationObject") == DdiFailureGroup::Queue);
    assert(native12::ddi_failure_group("pfnCreateFence") == DdiFailureGroup::Fence);
    assert(native12::ddi_failure_group("pfnSetFenceEventOnCompletion") == DdiFailureGroup::Fence);
    assert(native12::ddi_failure_group("pfnOpenSharedHandle") == DdiFailureGroup::Shared);
    assert(native12::ddi_failure_group("pfnOpenHeap") == DdiFailureGroup::Shared);
    assert(native12::ddi_failure_group("pfnOpenResource") == DdiFailureGroup::Shared);
    assert(native12::ddi_failure_group("pfnPresent") == DdiFailureGroup::Present);
    assert(native12::ddi_failure_group("pfnCheckFormatSupport") == DdiFailureGroup::Any);
    assert(native12::ddi_failure_group(nullptr) == DdiFailureGroup::Any);
    assert(!std::strcmp(native12::ddi_failure_group_name(DdiFailureGroup::Queue), "queue-sync"));
    assert(!std::strcmp(native12::ddi_failure_group_name(DdiFailureGroup::Present), "present"));

    // A refusal is a failure that is not the ordinary "not yet". This test takes no display DDI
    // header, so the removal code is written out here as ddi-entry.h's kDdiDriverDeviceRemoved
    // states it (D3DDDIERR_DEVICEREMOVED, 0x88760870).
    const HRESULT kDeviceRemoved = static_cast<HRESULT>(0x88760870);
    assert(native12::ddi_failure_is_refusal(E_FAIL));
    assert(native12::ddi_failure_is_refusal(kDeviceRemoved));
    assert(!native12::ddi_failure_is_refusal(S_OK));
    assert(!native12::ddi_failure_is_refusal(E_PENDING));

    // The budget, which is what is observable from here: a success and an E_PENDING cost nothing.
    const int32_t before_failures = native12::ddi_refusal_budget.load();
    native12::ddi_first_failure("pfnPresent", S_OK, false);
    native12::ddi_first_failure("pfnPresent", E_PENDING, false);
    assert(native12::ddi_refusal_budget.load() == before_failures);

    // The first failing DDI of the process writes ONE line, although it claims both the process-wide
    // slot and its own group: one refusal is one line.
    native12::ddi_first_failure("pfnExecuteCommandLists", E_FAIL, false);
    const int32_t after_first_failure = native12::ddi_refusal_budget.load();
    assert(before_failures - after_first_failure == 1);
    // The next refusal of the SAME group is silent: it is a consequence of the one above.
    native12::ddi_first_failure("pfnExecuteCommandLists", kDeviceRemoved, true);
    native12::ddi_first_failure("pfnCreateCommandQueue", E_OUTOFMEMORY, false);
    assert(native12::ddi_refusal_budget.load() == after_first_failure);
    // A refusal of ANOTHER group of the route's path still gets its line, because a present that
    // refused after a queue call that refused is a second reading and not the same one.
    native12::ddi_first_failure("pfnPresent", E_FAIL, true);
    assert(after_first_failure - native12::ddi_refusal_budget.load() == 1);
    const int32_t after_present = native12::ddi_refusal_budget.load();
    native12::ddi_first_failure("pfnPresent", E_FAIL, true);
    assert(native12::ddi_refusal_budget.load() == after_present);
    // Five groups and the process-wide one: at most six lines, whatever a game does.
    assert(unsigned(DdiFailureGroup::Count) == 6);

    std::puts("PASS ddi experiment sources: syntax, matching, profile key, machine-wide value, variable "
              "precedence, none, refusals, the -off form of every default, the one-shot first-removal line "
              "and the first failing DDI of each group of the presenter's path");
    return 0;
}
