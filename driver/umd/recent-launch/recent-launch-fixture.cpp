// SPDX-License-Identifier: MIT
// Host test fixture of recent-launch.h (BD-315): a DLL that compiles the helper in, as each shell does, so that
// recent-launch-test.exe can load it, make one note through it and unload it. The test then checks that the launch
// mark of that note is closed with the module and does not stay open until the process ends.
#include "recent-launch.h"

namespace rl=amdgpu_wddm::recent_launch;

extern "C" __declspec(dllexport) unsigned FixtureCommit(const wchar_t* store,const wchar_t* switch_key,const wchar_t* exe,
                                                        const wchar_t* windows,unsigned pid,unsigned long long start,
                                                        unsigned api) noexcept {
    try {
        rl::Inputs in;
        in.store_dir=store;in.switch_key=switch_key;in.exe=exe;in.windows_dir=windows;in.pid=pid;in.start=start;
        return static_cast<unsigned>(rl::commit(in,api));
    } catch(...) {return ~0u;}
}

extern "C" __declspec(dllexport) unsigned FixtureMarks() noexcept {return static_cast<unsigned>(rl::held_marks.count());}
