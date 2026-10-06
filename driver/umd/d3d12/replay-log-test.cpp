// SPDX-License-Identifier: MIT
// The replay-log file (replay-log.h): its name, its directory without a profile, one line per write with the UTC
// time, the QPC and the thread, appends from two writers, a long line cut to one line, and a path that cannot be
// opened. Files go to the current directory (the build directory) and are deleted after.
#include "replay-log.h"
#include <cstdio>
#include <string>
#include <thread>

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
static std::string read_all(const wchar_t* path) {
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) return text;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) text.append(buffer, n);
    std::fclose(f);
    return text;
}
static size_t count(const std::string& text, const char* what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

int main() {
    namespace detail = native12::replay_log_detail;
    wchar_t out[MAX_PATH]{};
    check(detail::path(L"C:\\BC250\\tmp", L"C:\\Games\\bin\\x64_dx12\\witcher3.exe", 4242, 1, out, MAX_PATH) &&
              !std::wcscmp(out, L"C:\\BC250\\tmp\\amdgpu_wddm-replay-witcher3.exe-4242-1.log"),
          "name: directory without a separator, image file name, process id, device number");
    check(detail::path(L"D:/logs/", L"game.exe", 7, 2, out, MAX_PATH) &&
              !std::wcscmp(out, L"D:/logs/amdgpu_wddm-replay-game.exe-7-2.log"),
          "name: directory with a separator, a bare image name");
    check(!detail::path(L"", L"game.exe", 7, 1, out, MAX_PATH) && !detail::path(L"C:\\x", L"C:\\Games\\", 7, 1, out, MAX_PATH) &&
              !detail::path(L"C:\\x", L"game.exe", 7, 1, out, 20),
          "name: refused for an empty directory, an image path without a file name, or too little room");

    // Without a profile for this image (the test creates none): the temporary directory.
    wchar_t directory[MAX_PATH]{}, temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    check(detail::directory(directory, MAX_PATH) && !std::wcscmp(directory, temp),
          "directory: the temporary directory without a profile LogDirectory");

    wchar_t here[MAX_PATH]{}, file[2 * MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, here);
    check(detail::path(here, L"replay-log-test.exe", GetCurrentProcessId(), 1, file, 2 * MAX_PATH), "name: the test's file");
    DeleteFileW(file);
    {
        native12::ReplayLog a, b;
        check(a.open(file) && b.open(file) && a.is_open() && b.is_open(), "open: two writers on one file, shared");
        std::thread one([&] {
            for (int k = 0; k < 200; ++k) a.write("replay: line of writer one");
        });
        std::thread two([&] {
            for (int k = 0; k < 200; ++k) b.write("replay: line of writer two");
        });
        one.join();
        two.join();
        const std::string long_line(1000, 'x');
        a.write(long_line.c_str());
        a.write(nullptr);
    }
    const std::string text = read_all(file);
    check(count(text, "\n") == 401 && count(text, " replay: line of writer one\n") == 200 &&
              count(text, " replay: line of writer two\n") == 200,
          "write: 400 whole lines from two writers and the long line cut to one line, a null line skipped");
    const size_t first = text.find('\n');
    const std::string line = text.substr(0, first);
    check(line.size() > 40 && line[4] == '-' && line[10] == 'T' && line[23] == 'Z' && line.find(" qpc=") == 24 &&
              line.find(" thread=") != std::string::npos,
          "write: each line starts with the UTC time, the QPC and the thread");
    DeleteFileW(file);

    native12::ReplayLog none;
    none.write("nothing");
    const std::wstring missing = std::wstring(here) + L"\\replay-log-test-missing-directory\\x.log";
    check(!none.open(missing.c_str()) && !none.is_open(),
          "open: a path that cannot be opened gives no file, and writes without a file do nothing");

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
