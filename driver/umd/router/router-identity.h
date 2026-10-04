// SPDX-License-Identifier: MIT
// File identity for the Windows-component test of the AppRouter policy (router-policy.h). Kept apart from the
// policy because it does I/O. GetModuleFileNameW and GetSystemWindowsDirectoryW return paths as they were spelled:
// an extended "\\?\" prefix, 8.3 short names or a junction into the Windows directory all defeat a plain prefix
// test. Both sides are therefore opened and resolved to their final normalized path
// (GetFinalPathNameByHandleW, "\\?\C:\..." form) before IsWindowsComponentPath compares them. A path that cannot
// be resolved is Unknown, never a definite "not a component": the policy keeps an Unknown image on the CPU UMD
// in gpu-default mode unless Allow names it.
#pragma once
#include <windows.h>
#include "router-policy.h"

namespace bc250router {

// Final normalized DOS path of a file or directory into out (chars, including the NUL). False on any failure.
inline bool ResolveFinalPath(const wchar_t *path, wchar_t *out, size_t chars)
{
    if (!path || !*path || !out || chars < 2 || chars > MAXDWORD) return false;
    // FILE_READ_ATTRIBUTES only; BACKUP_SEMANTICS lets the same call open a directory.
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const DWORD n = GetFinalPathNameByHandleW(h, out, (DWORD)chars, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    return n && n < chars;
}

// image: the process image as GetModuleFileNameW returned it. windows_dir: GetSystemWindowsDirectoryW's result,
// or nullptr/empty when that call failed or overflowed.
inline Component ClassifyComponent(const wchar_t *image, const wchar_t *windows_dir)
{
    const size_t chars = 1024;
    wchar_t final_image[chars], final_windows[chars];
    if (!ResolveFinalPath(image, final_image, chars)) return Component::Unknown;
    if (!ResolveFinalPath(windows_dir, final_windows, chars)) return Component::Unknown;
    return IsWindowsComponentPath(final_image, final_windows) ? Component::Yes : Component::No;
}

} // namespace bc250router
