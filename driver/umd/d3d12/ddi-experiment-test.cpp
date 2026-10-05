// SPDX-License-Identifier: MIT
// The experiment list and its sources (ddi-trace.h): the environment variable, the application profile in
// HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<image>, the "none" value and the refusals. The registry part is
// read only: this test creates no key, so its own image has no profile and resolves to None without the variable.
#include "ddi-trace.h"
#include <cassert>
#include <cstdio>
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

    // M15.14: the scan-out mode carries the geometry it is for in its own token, and nothing else is
    // the scan-out mode. A bare "scanout-flip" asks for nothing, so an operator who forgets the mode
    // gets the registered composed path rather than a chain moved into VRAM for a flip that the kernel
    // driver would refuse at every present.
    unsigned width = 0, height = 0;
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

    // Without the variable: this image has no profile.
    assert(SetEnvironmentVariableA(kVariable, nullptr));
    v = detail::resolve_experiment();
    assert(v.source == DdiExperimentSource::None && !v.text[0]);
    char text[256]{};
    assert(detail::application_profile(text, sizeof(text)) == 0 && !text[0]);

    std::puts("PASS ddi experiment sources: syntax, matching, profile key, variable precedence, none, refusals");
    return 0;
}
